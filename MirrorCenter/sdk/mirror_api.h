/**
 * mirror_api.h — MirrorCenter 投屏接收 SDK(C ABI)
 *
 * 用途:让任意语言/框架的第三方软件集成投屏接收能力。
 * 支持平台:Windows / Linux(x86_64)
 *
 * 对接方式:
 *   - Windows:LoadLibrary("mirrorsdk.dll") 或链接 mirrorsdk.lib
 *   - Linux:  dlopen("libmirrorsdk.so")
 *
 * 使用流程:
 *   1. mirror_init()
 *   2. mirror_start_session(...) → 得到 session 句柄
 *   3. 等待回调 MIRROR_STATE_WINDOW_READY(或轮询 mirror_get_window)
 *   4. 用 mirror_get_window 返回的句柄,在自己的框架中嵌入子进程窗口
 *      (Qt: QWindow::fromWinId + createWindowContainer;Win32: SetParent;GTK: gtk_plug)
 *   5. mirror_stop_session / mirror_shutdown
 *
 * 注意:
 *   - 所有回调都在 SDK 内部事件线程触发,宿主回调里不要做阻塞操作。
 *   - 回调触发时 session 指针仍有效;session 关闭后该指针作废。
 */
#ifndef MIRROR_API_H
#define MIRROR_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(MIRROR_SDK_BUILD)
#    define MIRROR_API __declspec(dllexport)
#  else
#    define MIRROR_API __declspec(dllimport)
#  endif
#else
#  define MIRROR_API __attribute__((visibility("default")))
#endif

/* 不透明会话句柄 */
typedef struct mirror_session mirror_session_t;

typedef enum mirror_result {
    MIRROR_OK = 0,
    MIRROR_ERR_INVALID_ARG = -1,
    MIRROR_ERR_NOT_INITIALIZED = -2,
    MIRROR_ERR_NOT_FOUND = -3,
    MIRROR_ERR_SESSION_FAILED = -4,
} mirror_result_t;

typedef enum mirror_backend {
    MIRROR_BACKEND_AIRPLAY = 0,   /* UxPlay(iOS AirPlay 接收) */
    MIRROR_BACKEND_MIRACAST = 1,  /* Miracast(安卓/笔记本) */
} mirror_backend_t;

typedef enum mirror_state {
    MIRROR_STATE_STARTING = 0,
    MIRROR_STATE_RUNNING = 1,
    MIRROR_STATE_WINDOW_READY = 2,   /* 子进程窗口已就绪,mirror_get_window 可用 */
    MIRROR_STATE_FAILED = 3,
    MIRROR_STATE_CLOSED = 4,
} mirror_state_t;

/* ---- 事件回调 ---- */

/* 会话状态变化。state == MIRROR_STATE_CLOSED 后 session 句柄作废。 */
typedef void (*mirror_state_callback)(mirror_session_t *session,
                                      mirror_state_t state,
                                      void *userdata);

/* 子进程窗口句柄就绪。handle 为 HWND(Windows)或 X11 Window ID(Linux),0 无效。 */
typedef void (*mirror_window_callback)(mirror_session_t *session,
                                       uint64_t handle,
                                       void *userdata);

/* 日志输出。message 仅在本次回调内有效。 */
typedef void (*mirror_log_callback)(mirror_session_t *session,
                                    const char *message,
                                    void *userdata);

/* 新帧到达回调(仅 Miracast 帧模式,宿主可在此用 mirror_get_frame 取帧)。 */
typedef void (*mirror_frame_callback)(mirror_session_t *session,
                                      void *userdata);

typedef struct mirror_callbacks {
    mirror_state_callback  on_state;
    mirror_window_callback on_window;
    mirror_log_callback    on_log;
    mirror_frame_callback  on_frame;
} mirror_callbacks_t;

/* ---- 网关(单广播名 + 多静默实例调度) ---- */

/* 网关日志。message 仅在本次回调内有效。 */
typedef void (*mirror_gateway_log_callback)(const char *message,
                                            void *userdata);

/* 设备连入/断开。session 为对应会话句柄,可直接用于 mirror_set_callbacks /
 * mirror_get_window 等查询;断开回调触发后该句柄随后失效,宿主应清理对应视图。 */
typedef void (*mirror_gateway_client_callback)(mirror_session_t *session,
                                               const char *client_ip,
                                               void *userdata);

/* 来源手机名称/型号就绪(客户端配对握手后上报, 可能晚于连入回调)。 */
typedef void (*mirror_gateway_client_info_callback)(mirror_session_t *session,
                                                    const char *client_name,
                                                    const char *client_model,
                                                    void *userdata);

/* 实例实际视频解码器就绪(如 "avdec_h264" 软解 / "d3d11h264dec" 硬解)。
 * 宿主可根据软/硬解能力决定最大同时投屏路数(软解 CPU 吃紧, 应减少路数)。 */
typedef void (*mirror_gateway_decoder_callback)(mirror_session_t *session,
                                                const char *decoder,
                                                void *userdata);

typedef struct mirror_gateway_callbacks {
    mirror_gateway_log_callback    on_log;
    mirror_gateway_client_callback on_client_connected;
    mirror_gateway_client_callback on_client_disconnected;
    /* 来源手机信息(名称/型号)就绪 */
    mirror_gateway_client_info_callback on_client_info;
    /* 实例实际视频解码器就绪(软/硬解判断) */
    mirror_gateway_decoder_callback on_decoder;
} mirror_gateway_callbacks_t;

/*
 * 启动 AirPlay 网关:
 *   - 以唯一设备名(默认 MirrorCenter)广播 mDNS, 监听 gatewayPort=7100
 *   - 新设备连入时按需启动静默 uxplay 实例(-silent), 透明转发 RTSP
 *   - 设备断开后实例保留 30s, 期间同设备重连续用原实例
 * @param device_name 广播设备名(可为 NULL, 默认 "MirrorCenter")
 * @param exe_path    uxplay 可执行文件路径(可为 NULL, SDK 自动搜索)
 * @param keyfile     共享配对密钥文件(可为 NULL, 默认应用目录 mirrorcenter.key)
 * @param mac         固定 MAC(可为 NULL, 默认 6c:6c:1b:30:00:01)
 * @param cbs         网关回调(可为 NULL)
 * @param userdata    回调用户数据
 */
MIRROR_API mirror_result_t mirror_start_airplay_gateway(
    const char *device_name,
    const char *exe_path,
    const char *keyfile,
    const char *mac,
    const mirror_gateway_callbacks_t *cbs,
    void *userdata);

/* 停止 AirPlay 网关,销毁所有实例。 */
MIRROR_API mirror_result_t mirror_stop_airplay_gateway(void);

/* ---- 生命周期 ---- */

/* 初始化 SDK。可调用多次,幂等。返回 MIRROR_OK 或错误码。 */
MIRROR_API mirror_result_t mirror_init(void);

/* 反初始化,停止所有会话并释放资源。 */
MIRROR_API void mirror_shutdown(void);

/* ---- 会话控制 ---- */

/**
 * 启动一个投屏接收会话。
 * @param backend    后端类型
 * @param device_name 会话显示名(可为 NULL)
 * @param exe_path   后端可执行文件路径(可为 NULL,SDK 会尝试搜索内置路径)
 * @param args       附加命令行参数,空格分隔(可为 NULL)
 * @param cbs        事件回调(可为 NULL)
 * @param userdata   回调用户数据(可为 NULL)
 * @param out_session 输出会话句柄
 */
MIRROR_API mirror_result_t mirror_start_session(mirror_backend_t backend,
                                                const char *device_name,
                                                const char *exe_path,
                                                const char *args,
                                                const mirror_callbacks_t *cbs,
                                                void *userdata,
                                                mirror_session_t **out_session);

/* 停止指定会话。 */
MIRROR_API mirror_result_t mirror_stop_session(mirror_session_t *session);

/* 停止所有会话。 */
MIRROR_API mirror_result_t mirror_stop_all(void);

/* 销毁会话句柄(停止并释放)。等价于 mirror_stop_session + 释放。 */
MIRROR_API mirror_result_t mirror_destroy_session(mirror_session_t *session);

/* ---- 查询 ---- */

/* 获取会话窗口句柄:Windows HWND / Linux X11 Window ID。未就绪返回 0。 */
MIRROR_API uint64_t mirror_get_window(mirror_session_t *session);

/* 获取后端子进程 PID(0 = 未启动)。可用于按会话控制音频(如静音)。 */
MIRROR_API uint64_t mirror_get_process_id(mirror_session_t *session);

/* 获取会话当前状态。 */
MIRROR_API mirror_state_t mirror_get_state(mirror_session_t *session);

/* 获取会话显示名(返回 SDK 内部字符串,勿释放;无则返回 NULL)。 */
MIRROR_API const char *mirror_get_device_name(mirror_session_t *session);

/* 帧数据(仅 Miracast 帧模式)。data 为 BGRA8,stride 为每行字节数。 */
typedef struct mirror_frame {
    int width;
    int height;
    int stride;
    const uint8_t *data;   /* 指向 SDK 内部缓冲,仅在调用期间有效 */
} mirror_frame_t;

/*
 * 获取最新一帧。返回 MIRROR_OK 且有新帧时填充 frame;
 * 返回 MIRROR_ERR_NOT_FOUND 表示尚无帧。
 */
MIRROR_API mirror_result_t mirror_get_frame(mirror_session_t *session,
                                            mirror_frame_t *frame);

/* 设置/更新会话回调。 */
MIRROR_API mirror_result_t mirror_set_callbacks(mirror_session_t *session,
                                                const mirror_callbacks_t *cbs,
                                                void *userdata);

/* 获取错误说明(线程本地缓冲,仅供调试)。 */
MIRROR_API const char *mirror_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* MIRROR_API_H */
