#include "d3dinterop.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QDebug>

#include <d3d11.h>
#include <d3d11_1.h>    // ID3D11Device1::OpenSharedResourceByName
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// WGL_NV_DX_interop 访问权标记(wglext.h 值, 不引全头避免与 Qt GL 冲突)
static const GLenum kAccessReadOnly = 0x0000;   // WGL_ACCESS_READ_ONLY_NV

namespace mirror {

namespace {
// 进程级探测缓存(-1 未测, 0 不支持, 1 支持)
int g_probeResult = -1;

// 规范化 GPU 名称用于匹配: 去 (R)/(TM) 标记、斜杠转空格、压缩空白。
// GL 渲染器串如 "NVIDIA GeForce GTX 1660 Ti/PCIe/SSE2",
// DXGI 适配器名如 "NVIDIA GeForce GTX 1660 Ti" / "Intel(R) UHD Graphics 630"。
QString normalizeGpuName(const QString &s)
{
    QString r = s;
    r.remove(QStringLiteral("(R)"), Qt::CaseInsensitive);
    r.remove(QStringLiteral("(TM)"), Qt::CaseInsensitive);
    r.replace(QLatin1Char('/'), QLatin1Char(' '));
    return r.simplified();
}
}

D3DInterop::~D3DInterop()
{
    // 析构时 GL 上下文不一定 current, 无法安全注销 —— 正常路径必须先调 close()。
    if (m_wglDev || m_d3dDev)
        qWarning() << "[dxinterop] destroyed without close() (context change leak, tolerated)";
}

bool D3DInterop::probeSupported()
{
    if (g_probeResult >= 0)
        return g_probeResult == 1;

    // 离屏上下文 + wglGetProcAddress 探测。桌面 GL 才有 WGL 扩展;
    // ANGLE/D3D 后端的 QOpenGLContext 下 wglGetProcAddress 拿不到 NV 扩展。
    bool ok = false;
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext ctx;
    if (surface.isValid() && ctx.create() && ctx.makeCurrent(&surface)) {
        // wglGetProcAddress 必须在有当前 WGL 上下文时调用
        auto pOpen = reinterpret_cast<PFDXOpenDevice>(
            wglGetProcAddress("wglDXOpenDeviceNV"));
        auto pRegister = reinterpret_cast<PFDXRegisterObject>(
            wglGetProcAddress("wglDXRegisterObjectNV"));
        auto pLock = reinterpret_cast<PFDXLockObjects>(
            wglGetProcAddress("wglDXLockObjectsNV"));
        ok = pOpen && pRegister && pLock;
        ctx.doneCurrent();
        if (ok)
            qInfo() << "[dxinterop] WGL_NV_DX_interop2 supported";
        else
            qInfo() << "[dxinterop] WGL_NV_DX_interop2 NOT available (SHM fallback)";
    } else {
        qInfo() << "[dxinterop] probe: no offscreen GL context (SHM fallback)";
    }
    surface.destroy();
    g_probeResult = ok ? 1 : 0;
    return ok;
}

bool D3DInterop::ensureFunctions()
{
    if (m_pOpen)
        return true;
    if (!wglGetCurrentContext()) {
        qWarning() << "[dxinterop] ensureFunctions: no current WGL context";
        return false;
    }
    m_pOpen = reinterpret_cast<PFDXOpenDevice>(wglGetProcAddress("wglDXOpenDeviceNV"));
    m_pClose = reinterpret_cast<PFDXCloseDevice>(wglGetProcAddress("wglDXCloseDeviceNV"));
    m_pRegister = reinterpret_cast<PFDXRegisterObject>(wglGetProcAddress("wglDXRegisterObjectNV"));
    m_pUnregister = reinterpret_cast<PFDXUnregisterObject>(wglGetProcAddress("wglDXUnregisterObjectNV"));
    m_pLock = reinterpret_cast<PFDXLockObjects>(wglGetProcAddress("wglDXLockObjectsNV"));
    m_pUnlock = reinterpret_cast<PFDXUnlockObjects>(wglGetProcAddress("wglDXUnlockObjectsNV"));
    if (!m_pOpen || !m_pClose || !m_pRegister || !m_pUnregister || !m_pLock || !m_pUnlock) {
        qWarning() << "[dxinterop] wglDX* function missing";
        return false;
    }
    return true;
}

bool D3DInterop::open()
{
    if (m_wglDev)
        return true;
    if (!ensureFunctions())
        return false;

    // 多适配器机器(如 Intel+NVIDIA+虚拟显示适配器)上, GL 上下文所在 GPU 未必是
    // DXGI 默认适配器; 跨适配器注册 wglDXRegisterObjectNV 会失败。
    // 因此先取 GL 渲染器字符串, 枚举 DXGI 适配器找到同一块物理 GPU 再建设备。
    auto *ctx = QOpenGLContext::currentContext();
    QString glRenderer;
    if (ctx) {
        if (const GLubyte *r = ctx->functions()->glGetString(GL_RENDERER))
            glRenderer = QString::fromLatin1(reinterpret_cast<const char *>(r));
    }
    const QString glNorm = normalizeGpuName(glRenderer);

    IDXGIAdapter1 *chosen = nullptr;
    QString chosenName;
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void **>(&factory))) && factory) {
        for (UINT i = 0; !chosen; ++i) {
            IDXGIAdapter1 *ad = nullptr;
            if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(ad->GetDesc1(&d))
                && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                const QString desc = normalizeGpuName(QString::fromWCharArray(d.Description));
                const QStringList toks = desc.split(QLatin1Char(' '));
                const QString key = toks.mid(0, 3).join(QLatin1Char(' '));
                if (!glNorm.isEmpty() && !key.isEmpty() && glNorm.contains(key)) {
                    chosen = ad;          // 生命周期保持到 CreateDevice 后释放
                    chosenName = desc;
                    continue;
                }
            }
            ad->Release();
        }
        factory->Release();
    }
    if (chosen)
        qInfo() << "[dxinterop] GL renderer" << glRenderer << "→ adapter" << chosenName;
    else
        qInfo() << "[dxinterop] GL renderer" << glRenderer << "→ default adapter";

    // 宿主自建 D3D11 设备(优先 GL 同适配器; BGRA 支持与 M1 探测一致)
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device *dev = nullptr;
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN
                                                  : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, &fl, nullptr);
    if (chosen)
        chosen->Release();
    if (FAILED(hr) || !dev) {
        qWarning() << "[dxinterop] D3D11CreateDevice failed hr=0x"
                   << QString::number(static_cast<uint>(hr), 16);
        return false;
    }
    m_d3dDev = dev;
    dev->GetImmediateContext(reinterpret_cast<ID3D11DeviceContext **>(&m_ctx));

    m_wglDev = m_pOpen(dev);
    if (!m_wglDev) {
        qWarning() << "[dxinterop] wglDXOpenDeviceNV failed";
        dev->Release();
        m_d3dDev = nullptr;
        return false;
    }
    qInfo() << "[dxinterop] opened (d3d device" << fl << ")";
    return true;
}

GLuint D3DInterop::openSharedTexture(const wchar_t *name, int *outW, int *outH, void **outObj)
{
    *outObj = nullptr;
    *outW = *outH = 0;
    if (!m_wglDev) {
        qWarning() << "[dxinterop] openSharedTexture: interop not open";
        return 0;
    }
    auto *dev = static_cast<ID3D11Device *>(m_d3dDev);
    ID3D11Device1 *dev1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1)))
        || !dev1) {
        qWarning() << "[dxinterop] QI ID3D11Device1 failed";
        return 0;
    }

    // 按名打开(跨设备合法; 只需读权限)。名字对象生命周期由服务端句柄维持。
    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = dev1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ,
                                                __uuidof(ID3D11Texture2D),
                                                reinterpret_cast<void **>(&tex));
    dev1->Release();
    if (FAILED(hr) || !tex) {
        qWarning() << "[dxinterop] OpenSharedResourceByName failed hr=0x"
                   << QString::number(static_cast<uint>(hr), 16)
                   << QString::fromWCharArray(name);
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    *outW = static_cast<int>(desc.Width);
    *outH = static_cast<int>(desc.Height);

    // 服务端纹理是按名打开的 SHARED_NTHANDLE 资源, NVIDIA 驱动拒绝将其直接
    // 注册给 interop(真机实测 GLE=0)。改为本地建同尺寸影子纹理(MiscFlags=0,
    // 本地纹理注册是 interop 的标准路径), 每帧 blit() 由设备内 CopyResource
    // 拷入, 仍无 CPU 参与。
    D3D11_TEXTURE2D_DESC shadowDesc = desc;
    shadowDesc.MiscFlags = 0;
    ID3D11Texture2D *shadow = nullptr;
    hr = dev->CreateTexture2D(&shadowDesc, nullptr, &shadow);
    if (FAILED(hr) || !shadow) {
        qWarning() << "[dxinterop] create shadow texture failed hr=0x"
                   << QString::number(static_cast<uint>(hr), 16);
        tex->Release();
        return 0;
    }

    auto *glf = QOpenGLContext::currentContext()->functions();
    GLuint glTex = 0;
    glf->glGenTextures(1, &glTex);
    HANDLE obj = m_pRegister(m_wglDev, shadow, glTex, GL_TEXTURE_2D, kAccessReadOnly);
    if (!obj) {
        const DWORD err = GetLastError();   // 注册失败根因定位(紧随调用捕获)
        qWarning() << "[dxinterop] wglDXRegisterObjectNV failed for"
                   << QString::fromWCharArray(name)
                   << "GLE=0x" << QString::number(err, 16)
                   << "desc: usage" << desc.Usage << "bind" << desc.BindFlags
                   << "misc" << Qt::hex << desc.MiscFlags
                   << "fmt" << Qt::hex << desc.Format;
        glf->glDeleteTextures(1, &glTex);
        shadow->Release();
        tex->Release();
        return 0;
    }
    // 两个 D3D 纹理引用都保留到注销(releaseTexture/close 统一 Release),
    // 防注册对象内部借用悬空; 服务端纹理是 blit 的拷贝源, 必须保活。

    // 采样必需: 注册纹理默认无 mipmap, 默认 MIN 过滤(GL_NEAREST_MIPMAP)不完整
    glf->glBindTexture(GL_TEXTURE_2D, glTex);
    glf->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glf->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glf->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glf->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glf->glBindTexture(GL_TEXTURE_2D, 0);

    m_texByObj.insert(obj, { glTex, tex, shadow });

    *outObj = obj;
    return glTex;
}

bool D3DInterop::blit(void *obj)
{
    auto it = m_texByObj.find(obj);
    if (it == m_texByObj.end() || !m_ctx)
        return false;
    auto *dst = static_cast<ID3D11Texture2D *>(it->shadow);
    auto *src = static_cast<ID3D11Texture2D *>(it->server);
    static_cast<ID3D11DeviceContext *>(m_ctx)->CopyResource(dst, src);
    return true;
}

void D3DInterop::releaseTexture(GLuint *tex, void **obj)
{
    if (*obj && m_pUnregister && m_wglDev) {
        HANDLE o = static_cast<HANDLE>(*obj);
        auto it = m_texByObj.find(o);
        if (it != m_texByObj.end()) {
            if (!m_pUnregister(m_wglDev, o))
                qWarning() << "[dxinterop] wglDXUnregisterObjectNV failed";
            if (it->shadow)
                static_cast<ID3D11Texture2D *>(it->shadow)->Release();
            if (it->server)
                static_cast<ID3D11Texture2D *>(it->server)->Release();
            m_texByObj.erase(it);
        }
        *obj = nullptr;
    }
    if (*tex) {
        if (auto *glf = QOpenGLContext::currentContext()
                            ? QOpenGLContext::currentContext()->functions() : nullptr)
            glf->glDeleteTextures(1, tex);
        *tex = 0;
    }
}

bool D3DInterop::lock(void *obj)
{
    if (!obj || !m_wglDev || !m_pLock)
        return false;
    HANDLE o = static_cast<HANDLE>(obj);
    return m_pLock(m_wglDev, 1, &o) != FALSE;
}

void D3DInterop::unlock(void *obj)
{
    if (!obj || !m_wglDev || !m_pUnlock)
        return;
    HANDLE o = static_cast<HANDLE>(obj);
    m_pUnlock(m_wglDev, 1, &o);
}

bool D3DInterop::readTexture(void *obj, QImage &out)
{
    out = QImage();
    if (!obj || !m_d3dDev || !m_ctx)
        return false;
    const auto it = m_texByObj.constFind(obj);
    if (it == m_texByObj.constEnd() || !it->shadow)
        return false;
    auto *shadow = static_cast<ID3D11Texture2D *>(it->shadow);
    D3D11_TEXTURE2D_DESC desc;
    shadow->GetDesc(&desc);

    auto *dev = static_cast<ID3D11Device *>(m_d3dDev);
    auto *ctx = static_cast<ID3D11DeviceContext *>(m_ctx);
    if (!m_staging || m_stagingW != int(desc.Width) || m_stagingH != int(desc.Height)) {
        if (m_staging) {
            static_cast<ID3D11Texture2D *>(m_staging)->Release();
            m_staging = nullptr;
        }
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr,
                                        reinterpret_cast<ID3D11Texture2D **>(&m_staging)))
            || !m_staging)
            return false;
        m_stagingW = int(desc.Width);
        m_stagingH = int(desc.Height);
    }
    auto *stg = static_cast<ID3D11Texture2D *>(m_staging);
    ctx->CopyResource(stg, shadow);

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
        return false;
    QImage img(int(desc.Width), int(desc.Height), QImage::Format_RGB32);
    const int rowBytes = qMin<int>(int(map.RowPitch), img.bytesPerLine());
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(img.scanLine(int(y)),
               reinterpret_cast<const uchar *>(map.pData) + size_t(y) * map.RowPitch,
               rowBytes);
    ctx->Unmap(stg, 0);
    out = img;
    return true;
}

void D3DInterop::close()
{
    // 要求 GL 上下文 current(注销/删纹理需要), 调用方保证;
    // 无 current 上下文时只能释放 D3D 引用(GL 名泄漏, 容忍于上下文重建场景)。
    auto *glf = QOpenGLContext::currentContext()
                    ? QOpenGLContext::currentContext()->functions() : nullptr;
    QList<GLuint> glNames;
    for (auto it = m_texByObj.begin(); it != m_texByObj.end(); ++it) {
        if (m_wglDev && m_pUnregister)
            m_pUnregister(m_wglDev, static_cast<HANDLE>(it.key()));
        if (it->shadow)
            static_cast<ID3D11Texture2D *>(it->shadow)->Release();
        if (it->server)
            static_cast<ID3D11Texture2D *>(it->server)->Release();
        if (glf && it->glName)
            glNames.append(it->glName);
    }
    m_texByObj.clear();
    if (glf) {
        for (GLuint n : glNames)
            glf->glDeleteTextures(1, &n);
    }
    if (m_staging) {
        static_cast<ID3D11Texture2D *>(m_staging)->Release();
        m_staging = nullptr;
    }
    m_stagingW = m_stagingH = 0;
    if (m_ctx) {
        static_cast<ID3D11DeviceContext *>(m_ctx)->Release();
        m_ctx = nullptr;
    }
    if (m_wglDev && m_pClose) {
        m_pClose(m_wglDev);
        m_wglDev = nullptr;
    }
    if (m_d3dDev) {
        static_cast<ID3D11Device *>(m_d3dDev)->Release();
        m_d3dDev = nullptr;
    }
}

} // namespace mirror
