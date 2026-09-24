#pragma once

// WGL_NV_DX_interop2 封装:把 D3D11 纹理注册为 OpenGL 纹理直接采样
// (Miracast GPU 零拷贝渲染, M2)。
//
// 链路: 服务端(MiracastReceiverService)按名发布共享纹理
//   Local\MirrorCenterSharedTex_<port>_g<gen>_<slot>
// 宿主在此: D3D11CreateDevice → OpenSharedResourceByName → 建本地影子纹理
//   (MiscFlags=0)→ 影子注册为 GL 纹理 → 每帧 blit() GPU 内 CopyResource
//   → paintGL 中 wglDXLockObjectsNV → 采样 → wglDXUnlockObjectsNV。
//
// 影子纹理的由来(2026-09-24 真机实测): NVIDIA 驱动拒绝注册按名打开的
// SHARED_NTHANDLE 共享纹理(wglDXRegisterObjectNV 直接失败 GLE=0, 同适配器
// 亦然) —— 服务端纹理无法直接注册。标准做法(Spout 同款): 本地建同尺寸纹理
// 注册给 interop, 每帧设备内 CopyResource 一跳(GPU-GPU, 无 CPU 参与,
// 1080p ≈ 0.3ms), 保持零 CPU 拷贝。
//
// 规约(来自 WGL_NV_DX_interop2 spec):
//   - D3D11 资源无需 wglDXSetResourceShareHandleNV(调用无效但不出错)
//   - 可注册资源 Usage 必须为 D3D11_USAGE_DEFAULT(影子纹理满足)
//   - 注册后纹理存储由 D3D 拥有, 禁止 glTexImage*D; 采样前必须 lock
//
// 所有实例方法必须在 QOpenGLWidget 的 GL 上下文 current 的线程调用
// (GLFrameSurface 的 paintGL/initializeGL 满足)。需要桌面 OpenGL
// (WGL); Qt 走 ANGLE 时 supported()/open() 失败 → 宿主回退 SHM 路径。

#include <windows.h>
#include <QtGui/qopengl.h>   // GLuint/GLenum/GLint(GL 类型, 不引系统 gl.h)
#include <QHash>
#include <QImage>

namespace mirror {

class D3DInterop
{
public:
    D3DInterop() = default;
    ~D3DInterop();

    D3DInterop(const D3DInterop &) = delete;
    D3DInterop &operator=(const D3DInterop &) = delete;

    /// 探测运行环境是否支持 interop(需要桌面 GL 上下文; 内部建离屏上下文,
    /// 结果进程内缓存)。用于决定是否给接收服务传 --gpu 1。
    static bool probeSupported();

    /// 打开 interop 设备: 在当前适配器创建 D3D11 设备(FL 11.0 + BGRA)并
    /// wglDXOpenDeviceNV 绑定到当前 GL 上下文。必须在 GL 上下文 current 时调用。
    bool open();

    /// 按名打开共享纹理并注册为 GL 纹理(经本地影子纹理, 见文件头说明)。
    /// 返回 GL 纹理名(0=失败)。成功后已设置 LINEAR 过滤 + CLAMP_TO_EDGE
    /// (采样必需, 默认 mipmap 过滤会让纹理不完整)。outW/outH 返回纹理尺寸,
    /// outObj 返回 interop 对象句柄(blit/lock/unlock/unregister 用)。
    GLuint openSharedTexture(const wchar_t *name, int *outW, int *outH, void **outObj);

    /// 每帧把服务端共享纹理拷入影子纹理(GPU 内 CopyResource, 无 CPU 参与)。
    /// 必须在 lock(obj) 之前调用。失败返回 false, 调用方跳帧。
    bool blit(void *obj);

    /// 读回注册纹理当前内容到 QImage(黑边检测用, 秒级一次; 非逐帧路径)。
    /// CopyResource(staging←影子) + Map 读回, BGRA→Format_RGB32 逐行拷贝。
    /// 成功返回 true 并填充 out; obj 必须是 openSharedTexture 返回的句柄。
    bool readTexture(void *obj, QImage &out);

    /// 注销 interop 对象并删除 GL 纹理名(含影子/服务端纹理 D3D 引用)。
    /// tex/obj 置 0。
    void releaseTexture(GLuint *tex, void **obj);

    /// 锁定(采样前)。失败(被 D3D 侧持锁/纹理已销毁)返回 false, 调用方跳帧。
    bool lock(void *obj);
    /// 解锁(采样后)。
    void unlock(void *obj);

    /// 关闭 interop(注销全部对象、释放 D3D 设备)。须在 GL 上下文 current 时
    /// 调用(注销对象需要); GL 上下文重建时先调用本方法再重新 open。
    void close();

private:
    bool ensureFunctions();

    // WGL_NV_DX_interop(2) 函数指针
    using PFDXOpenDevice = HANDLE(WINAPI *)(void *dxDevice);
    using PFDXCloseDevice = BOOL(WINAPI *)(HANDLE hDevice);
    using PFDXRegisterObject = HANDLE(WINAPI *)(HANDLE hDevice, void *dxObject,
                                                GLuint name, GLenum type, GLenum access);
    using PFDXUnregisterObject = BOOL(WINAPI *)(HANDLE hDevice, HANDLE hObject);
    using PFDXLockObjects = BOOL(WINAPI *)(HANDLE hDevice, GLint count, HANDLE *hObjects);
    using PFDXUnlockObjects = BOOL(WINAPI *)(HANDLE hDevice, GLint count, HANDLE *hObjects);

    PFDXOpenDevice m_pOpen = nullptr;
    PFDXCloseDevice m_pClose = nullptr;
    PFDXRegisterObject m_pRegister = nullptr;
    PFDXUnregisterObject m_pUnregister = nullptr;
    PFDXLockObjects m_pLock = nullptr;
    PFDXUnlockObjects m_pUnlock = nullptr;

    HANDLE m_wglDev = nullptr;   // wglDXOpenDeviceNV 返回的互操作设备句柄
    void *m_d3dDev = nullptr;    // 宿主自建 ID3D11Device*(引用由本类管理)
    void *m_ctx = nullptr;       // ID3D11DeviceContext*(immediate, blit 拷贝用)

    // readTexture 读回 staging(按纹理尺寸惰性创建, 尺寸变化时重建)
    void *m_staging = nullptr;   // ID3D11Texture2D*(USAGE_STAGING, CPU 读)
    int m_stagingW = 0, m_stagingH = 0;

    // interop 对象句柄 → {GL 纹理名, 服务端共享纹理, 本地影子纹理}
    // (D3D 指针均为 ID3D11Texture2D*, 引用由本类管理; 注销 interop 后一并
    // Release; GL 纹理名仅 close() 清理用)
    struct TexPair {
        GLuint glName = 0;
        void *server = nullptr;
        void *shadow = nullptr;
    };
    QHash<void *, TexPair> m_texByObj;
};

} // namespace mirror
