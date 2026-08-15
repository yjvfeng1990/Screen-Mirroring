#include "mirror_api.h"
#include "sessionmanager.h"
#include "mirrorsession.h"
#include "airplaygateway.h"
#include "mice/micebackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QMutex>
#include <QMetaObject>
#include <QTimer>
#include <QTcpServer>
#include <QFile>
#include <cstdio>
#include <QDebug>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <QDir>
#endif

using namespace mirror;

/* ============ 内部状态 ============ */

struct mirror_session {
    QPointer<MirrorSession> core;      // Qt 核心会话对象
    mirror_callbacks_t cbs{};
    void *userdata = nullptr;
    bool valid = false;
    QImage frameCache;                 // 最近一帧缓存(data 指针指向它)
    int airplayPortBase = 0;           // AirPlay 实例端口基址(0 = 未分配)
};

// 递归锁:mirror_start_session 等会持锁调用 registerHandle/isHandleValid 等辅助函数,
// 若用普通 QMutex 会在同线程重入时自死锁(表现为点击投屏后 UI 卡死)
static QRecursiveMutex g_mutex;
static SessionManager *g_manager = nullptr;   // 生命周期:随 SDK 事件线程,不跨线程删除
static QHash<mirror_session_t *, mirror_session_t *> g_sessions; // 指针登记表(防野指针)
static QThread *g_thread = nullptr;
static bool g_initialized = false;

/* ---- 网关(单广播名 + 多静默实例调度) ---- */
static AirPlayGateway *g_gateway = nullptr;       // 生命周期:事件线程,由 mirror_stop_airplay_gateway 释放
static mirror_gateway_callbacks_t g_gatewayCbs{};
static void *g_gatewayUserdata = nullptr;
static QSet<mirror_session_t *> g_gatewayHandles; // 网关创建的会话句柄(回收时统一释放)

/* ---- MS-MICE(基础设施投屏)接收端 ---- */
static MiceBackend *g_miceBackend = nullptr;                     // 生命周期:事件线程
static QHash<QByteArray, mirror_session_t *> g_miceHandles;      // sourceId → 会话句柄

/* 错误说明(线程本地) */
static thread_local char g_lastError[256];

static void setError(const char *fmt, const char *arg = nullptr)
{
    if (arg)
        snprintf(g_lastError, sizeof(g_lastError), fmt, arg);
    else
        snprintf(g_lastError, sizeof(g_lastError), "%s", fmt);
}

/* ============ 事件线程引导 ============ */

static QCoreApplication *ensureApp()
{
    if (QCoreApplication::instance())
        return QCoreApplication::instance();

    // 宿主没有 Qt 事件循环时,为 SDK 起一个独立线程跑 QCoreApplication
    static int argc = 0;
    static char *argv[] = { nullptr };
    auto *app = new QCoreApplication(argc, argv);
    app->setObjectName(QStringLiteral("mirrorsdk-internal-app"));

    g_thread = new QThread();
    app->moveToThread(g_thread);
    g_thread->start();
    QMetaObject::invokeMethod(app, [] { QCoreApplication::instance()->exec(); },
                              Qt::QueuedConnection);
    return app;
}

static void cleanupInternalApp()
{
    if (g_thread) {
        QMetaObject::invokeMethod(QCoreApplication::instance(),
                                  [] { QCoreApplication::instance()->quit(); },
                                  Qt::QueuedConnection);
        g_thread->quit();
        g_thread->wait(3000);
        delete g_thread;
        g_thread = nullptr;
    }
}

/* ============ 会话对象创建/销毁(在事件线程执行) ============ */

struct CreateArg {
    BackendType type;
    QString deviceName;
    QString exePath;
    QStringList args;
};

static void createSessionInThread(SessionManager *mgr, const CreateArg &arg,
                                  mirror_session_t *handle, bool *ok)
{
    MirrorSession *core = mgr->createSession(arg.type, arg.deviceName,
                                             arg.exePath, arg.args);
    handle->core = core;
    *ok = (core != nullptr);
}

static void destroySessionInThread(mirror_session_t *handle)
{
    if (handle->core) {
        handle->core->stop();
        handle->core->deleteLater();
        handle->core = nullptr;
    }
}

/*
 * 在 g_manager 所在线程同步执行 fn。
 * 同线程时直接调用 —— Qt 6 对 functor + BlockingQueuedConnection 在同线程时
 * 仍会排入事件队列并触发 "Dead lock detected" 死锁检测(实际是误报),导致调用
 * 永远等待,UI 卡死。跨线程时才用 BlockingQueuedConnection 排队。
 */
template <typename F>
static void invokeOnManager(F &&fn)
{
    Q_ASSERT(g_manager);
    if (g_manager->thread() == QThread::currentThread()) {
        fn();
    } else {
        QMetaObject::invokeMethod(g_manager, std::forward<F>(fn),
                                  Qt::BlockingQueuedConnection);
    }
}

/* ============ 会话表管理 ============ */

// AirPlay 多实例端口分配:
// UxPlay 默认端口组(TCP 7100/7000/7001 + UDP 7011/6001/6000)同一进程只占一组,
// 多台苹果设备同时投屏需要每个会话启动独立的 uxplay 实例并分配互不冲突的端口组。
// 每组端口取连续 3 个(base, base+1, base+2),TCP/UDP 共用,步进 100 错开。
static constexpr int kAirplayPortBase = 7100;
static constexpr int kAirplayPortStep = 100;
static QSet<int> g_airplayPorts;          // 已分配端口组基址

// 分配最小未占用端口组基址(7100, 7200, 7300...)
static int allocAirplayPortBase()
{
    int base = kAirplayPortBase;
    while (g_airplayPorts.contains(base))
        base += kAirplayPortStep;
    g_airplayPorts.insert(base);
    return base;
}

static void freeAirplayPortBase(int base)
{
    g_airplayPorts.remove(base);
}

static bool registerHandle(mirror_session_t *h)
{
    QMutexLocker lock(&g_mutex);
    if (g_sessions.contains(h))
        return false;
    g_sessions.insert(h, h);
    return true;
}

static bool unregisterHandle(mirror_session_t *h)
{
    QMutexLocker lock(&g_mutex);
    return g_sessions.remove(h) != 0;
}

/* 校验句柄是否仍然有效(在锁保护下查找) */
static bool isHandleValid(mirror_session_t *h)
{
    QMutexLocker lock(&g_mutex);
    return g_sessions.contains(h);
}

/* ============ 回调分发 ============ */

static void dispatchState(mirror_session_t *h, SessionState s)
{
    if (!h || !h->cbs.on_state)
        return;
    mirror_state_t ms;
    switch (s) {
    case SessionState::Starting:   ms = MIRROR_STATE_STARTING; break;
    case SessionState::Running:    ms = MIRROR_STATE_RUNNING; break;
    case SessionState::WindowReady: ms = MIRROR_STATE_WINDOW_READY; break;
    case SessionState::Failed:     ms = MIRROR_STATE_FAILED; break;
    default:                       ms = MIRROR_STATE_CLOSED; break;
    }
    h->cbs.on_state(h, ms, h->userdata);
}

static void dispatchWindow(mirror_session_t *h, qulonglong w)
{
    if (h && h->cbs.on_window)
        h->cbs.on_window(h, static_cast<uint64_t>(w), h->userdata);
}

static void dispatchLog(mirror_session_t *h, const QString &msg)
{
    if (h && h->cbs.on_log) {
        QByteArray utf8 = msg.toUtf8();
        h->cbs.on_log(h, utf8.constData(), h->userdata);
    }
}

static void dispatchFrame(mirror_session_t *h)
{
    if (h && h->cbs.on_frame)
        h->cbs.on_frame(h, h->userdata);
}

/* ============ 网关辅助 ============ */

static mirror_session_t *findHandleForCore(MirrorSession *core)
{
    QMutexLocker lock(&g_mutex);
    for (auto *h : g_sessions.keys()) {
        if (h->core.data() == core)
            return h;
    }
    return nullptr;
}

/*
 * 连接网关信号 → SDK 会话句柄与宿主回调。
 * 网关对象在事件线程,连接使用直接连接 → 回调均在事件线程触发,
 * 宿主在回调里可安全调用 mirror_set_callbacks / mirror_get_window 等。
 */
static void connectGatewaySignals(AirPlayGateway *gw)
{
    // 设备连入:为后端会话创建 SDK 句柄并通知宿主
    QObject::connect(gw, &AirPlayGateway::clientConnected, gw,
                     [](const QString &ip, MirrorSession *session) {
        if (!session)
            return;
        auto *handle = new mirror_session_t();
        handle->core = session;
        handle->valid = true;
        if (!registerHandle(handle)) {
            delete handle;
            return;
        }
        {
            QMutexLocker lock(&g_mutex);
            g_gatewayHandles.insert(handle);
        }
        if (g_gatewayCbs.on_client_connected) {
            const QByteArray ipUtf8 = ip.toUtf8();
            g_gatewayCbs.on_client_connected(handle, ipUtf8.constData(),
                                             g_gatewayUserdata);
        }
    });

    // 设备断开:通知宿主,句柄随后失效
    QObject::connect(gw, &AirPlayGateway::clientDisconnected, gw,
                     [](const QString &ip, MirrorSession *session) {
        mirror_session_t *handle = findHandleForCore(session);
        if (!handle)
            return;
        if (g_gatewayCbs.on_client_disconnected) {
            const QByteArray ipUtf8 = ip.toUtf8();
            g_gatewayCbs.on_client_disconnected(handle, ipUtf8.constData(),
                                                g_gatewayUserdata);
        }
        {
            QMutexLocker lock(&g_mutex);
            g_gatewayHandles.remove(handle);
        }
        unregisterHandle(handle);
        handle->valid = false;
        handle->core = nullptr;
        delete handle;
    });

    QObject::connect(gw, &AirPlayGateway::clientInfoChanged, gw,
                     [](MirrorSession *session, const QString &name, const QString &model) {
        if (!g_gatewayCbs.on_client_info)
            return;
        mirror_session_t *handle = findHandleForCore(session);
        if (!handle)
            return;
        const QByteArray nameUtf8 = name.toUtf8();
        const QByteArray modelUtf8 = model.toUtf8();
        g_gatewayCbs.on_client_info(handle, nameUtf8.constData(),
                                    modelUtf8.constData(), g_gatewayUserdata);
    });

    // 实例实际视频解码器就绪(软/硬解判断)
    QObject::connect(gw, &AirPlayGateway::decoderChanged, gw,
                     [](MirrorSession *session, const QString &decoder) {
        if (!g_gatewayCbs.on_decoder)
            return;
        mirror_session_t *handle = findHandleForCore(session);
        if (!handle)
            return;
        const QByteArray decoderUtf8 = decoder.toUtf8();
        g_gatewayCbs.on_decoder(handle, decoderUtf8.constData(), g_gatewayUserdata);
    });

    QObject::connect(gw, &AirPlayGateway::logMessage, gw,
                     [](const QString &msg) {
        if (g_gatewayCbs.on_log) {
            const QByteArray utf8 = msg.toUtf8();
            g_gatewayCbs.on_log(utf8.constData(), g_gatewayUserdata);
        }
    });
}

/*
 * 停止网关并回收其会话句柄。可在持有 g_mutex 时调用(递归锁, 同线程安全)。
 * 已连接设备的视图清理依赖 on_state CLOSED 回调(网关停止会逐个关闭实例)。
 */
static void stopGatewayAndCleanHandles()
{
    AirPlayGateway *gw = nullptr;
    QList<mirror_session_t *> handles;
    {
        QMutexLocker lock(&g_mutex);
        gw = g_gateway;
        g_gateway = nullptr;
        g_gatewayCbs = mirror_gateway_callbacks_t{};
        g_gatewayUserdata = nullptr;
        handles = g_gatewayHandles.values();
        g_gatewayHandles.clear();
    }

    if (gw) {
        invokeOnManager([gw]() {
            gw->disconnect();
            gw->stop();
            gw->deleteLater();
        });
    }

    for (mirror_session_t *h : handles) {
        unregisterHandle(h);
        h->valid = false;
        h->core = nullptr;
        delete h;
    }
}

/*
 * 连接 MS-MICE 后端信号 → SDK 会话句柄与宿主回调。
 * 每路 Source 一个句柄;帧经 frameCache 缓存,宿主用 mirror_get_frame 拉取。
 */
static void connectMiceSignals(MiceBackend *backend)
{
    QObject::connect(backend, &MiceBackend::sourceConnected, backend,
                     [](const QString &friendlyName, const QByteArray &sourceId) {
        if (g_miceHandles.contains(sourceId))
            return;
        auto *handle = new mirror_session_t();
        handle->core = nullptr;   // MS-MICE 无后端子进程;帧走 frameCache
        handle->valid = true;
        if (!registerHandle(handle)) {
            delete handle;
            return;
        }
        {
            QMutexLocker lock(&g_mutex);
            g_miceHandles.insert(sourceId, handle);
        }
        if (g_gatewayCbs.on_client_connected) {
            const QByteArray nameUtf8 = friendlyName.toUtf8();
            // 第二个参数带 "MICE:" 前缀, 供 UI 区分 MS-MICE 会话与 AirPlay 网关
            const QByteArray tagged = "MICE:" + nameUtf8;
            g_gatewayCbs.on_client_connected(handle, tagged.constData(),
                                             g_gatewayUserdata);
        }
    });

    QObject::connect(backend, &MiceBackend::sourceDisconnected, backend,
                     [](const QByteArray &sourceId) {
        mirror_session_t *handle = nullptr;
        {
            QMutexLocker lock(&g_mutex);
            handle = g_miceHandles.take(sourceId);
        }
        if (!handle)
            return;
        if (g_gatewayCbs.on_client_disconnected) {
            g_gatewayCbs.on_client_disconnected(handle, "", g_gatewayUserdata);
        }
        unregisterHandle(handle);
        handle->valid = false;
        handle->core = nullptr;
        delete handle;
    });

    QObject::connect(backend, &MiceBackend::frameReady, backend,
                     [](const QByteArray &sourceId, const QImage &frame) {
        mirror_session_t *handle = nullptr;
        {
            QMutexLocker lock(&g_mutex);
            handle = g_miceHandles.value(sourceId);
        }
        if (!handle)
            return;
        {
            QMutexLocker lock(&g_mutex);
            handle->frameCache = frame;
        }
        if (handle->cbs.on_frame)
            handle->cbs.on_frame(handle, handle->userdata);
    });

    QObject::connect(backend, &MiceBackend::logMessage, backend,
                     [](const QString &msg) {
        if (g_gatewayCbs.on_log) {
            const QByteArray utf8 = msg.toUtf8();
            g_gatewayCbs.on_log(utf8.constData(), g_gatewayUserdata);
        }
    });
}

/* 停止 MS-MICE 后端并回收其会话句柄。 */
static void stopMiceAndCleanHandles()
{
    MiceBackend *backend = nullptr;
    QList<mirror_session_t *> handles;
    {
        QMutexLocker lock(&g_mutex);
        backend = g_miceBackend;
        g_miceBackend = nullptr;
        handles = g_miceHandles.values();
        g_miceHandles.clear();
    }

    if (backend) {
        invokeOnManager([backend]() {
            backend->disconnect();
            backend->stop();
            backend->deleteLater();
        });
    }

    for (mirror_session_t *h : handles) {
        unregisterHandle(h);
        h->valid = false;
        h->core = nullptr;
        delete h;
    }
}

/* ============ SDK 实现 ============ */

extern "C" {

MIRROR_API mirror_result_t mirror_init(void)
{
    qInfo() << "[sdk] mirror_init enter initialized=" << g_initialized;
    QMutexLocker lock(&g_mutex);
    if (g_initialized)
        return MIRROR_OK;

    ensureApp();
    QCoreApplication *app = QCoreApplication::instance();
    qInfo() << "[sdk] mirror_init app=" << app
            << "appThread=" << app->thread()
            << "current=" << QThread::currentThread();

    // 常驻 manager:由 mirror_shutdown 释放
    if (!g_manager) {
        g_manager = new SessionManager(nullptr);
        g_manager->moveToThread(app->thread());
    }
    Q_UNUSED(app)
    qInfo() << "[sdk] mirror_init managerThread=" << g_manager->thread()
            << "current=" << QThread::currentThread()
            << "appThread=" << app->thread();

    // 连接 manager 信号 → 会话句柄回调(在事件线程执行)
    QObject::connect(g_manager, &SessionManager::sessionStateChanged,
                     g_manager, [](const QString &id, SessionState s) {
        // 找到对应句柄
        QMutexLocker lock(&g_mutex);
        for (auto *h : g_sessions.keys()) {
            if (h->core && h->core->id() == id) {
                lock.unlock();
                dispatchState(h, s);
                lock.relock();
                break;
            }
        }
    }, Qt::QueuedConnection);

    QObject::connect(g_manager, &SessionManager::sessionWindowReady,
                     g_manager, [](const QString &id, qulonglong hwnd) {
        QMutexLocker lock(&g_mutex);
        for (auto *h : g_sessions.keys()) {
            if (h->core && h->core->id() == id) {
                lock.unlock();
                dispatchWindow(h, hwnd);
                lock.relock();
                break;
            }
        }
    }, Qt::QueuedConnection);

    QObject::connect(g_manager, &SessionManager::sessionLog,
                     g_manager, [](const QString &id, const QString &msg) {
        QMutexLocker lock(&g_mutex);
        for (auto *h : g_sessions.keys()) {
            if (h->core && h->core->id() == id) {
                lock.unlock();
                dispatchLog(h, msg);
                lock.relock();
                break;
            }
        }
    }, Qt::QueuedConnection);

    QObject::connect(g_manager, &SessionManager::sessionFrameReady,
                     g_manager, [](const QString &id) {
        QMutexLocker lock(&g_mutex);
        for (auto *h : g_sessions.keys()) {
            if (h->core && h->core->id() == id) {
                lock.unlock();
                dispatchFrame(h);
                lock.relock();
                break;
            }
        }
    }, Qt::QueuedConnection);

    QObject::connect(g_manager, &SessionManager::sessionRemoved,
                     g_manager, [](const QString &id) {
        Q_UNUSED(id)
    }, Qt::QueuedConnection);

    g_initialized = true;
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_start_airplay_gateway(
    const char *device_name,
    const char *exe_path,
    const char *keyfile,
    const char *mac,
    const mirror_gateway_callbacks_t *cbs,
    void *userdata)
{
    {
        QMutexLocker lock(&g_mutex);
        if (!g_initialized)
            return MIRROR_ERR_NOT_INITIALIZED;
        if (g_gateway)
            return MIRROR_OK;   // 已启动, 幂等
    }

    AirPlayGateway::Config cfg;
    cfg.deviceName = (device_name && *device_name)
                         ? QString::fromUtf8(device_name)
                         : QStringLiteral("MirrorCenter");
    cfg.keyfile = (keyfile && *keyfile)
                      ? QString::fromUtf8(keyfile)
                      : QDir(QCoreApplication::applicationDirPath())
                            .filePath(QStringLiteral("mirrorcenter.key"));
    cfg.mac = (mac && *mac) ? QString::fromUtf8(mac)
                            : QStringLiteral("6c:6c:1b:30:00:01");
    cfg.backendExe = (exe_path && *exe_path)
                         ? QString::fromUtf8(exe_path)
                         : g_manager->findBackendExe({QStringLiteral("uxplay.exe"),
                                                      QStringLiteral("uxplay")});
    // 与单会话模式一致:默认 d3d11videosink 输出(窗口嵌入依赖)。
    // 不追加 videosink_options:uxplay 会默认补 force-aspect-ratio=TRUE,
    // 每个格子独立等比显示完整画面(留边不裁切), 保证横竖屏都完整可见。
    cfg.extraArgs << QStringLiteral("-vs") << QStringLiteral("d3d11videosink");

    mirror_gateway_callbacks_t cbsCopy{};
    if (cbs)
        cbsCopy = *cbs;

    // 先挂回调:让网关启动期间的早期日志也能到达宿主
    {
        QMutexLocker lock(&g_mutex);
        g_gatewayCbs = cbsCopy;
        g_gatewayUserdata = userdata;
    }

    AirPlayGateway *gw = nullptr;
    invokeOnManager([&]() {
        auto *g = new AirPlayGateway(cfg, g_manager);
        connectGatewaySignals(g);
        if (!g->start()) {
            qWarning() << "[sdk] airplay gateway start failed";
            delete g;
            return;
        }
        gw = g;
    });
    if (!gw) {
        QMutexLocker lock(&g_mutex);
        g_gatewayCbs = mirror_gateway_callbacks_t{};
        g_gatewayUserdata = nullptr;
        setError("airplay gateway start failed");
        return MIRROR_ERR_SESSION_FAILED;
    }

    {
        QMutexLocker lock(&g_mutex);
        g_gateway = gw;
    }
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_stop_airplay_gateway(void)
{
    stopGatewayAndCleanHandles();
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API const char *mirror_airplay_fill_file(void)
{
    static const QByteArray path =
        QDir::temp().filePath(QStringLiteral("mirrorcenter_uxplay_fill.txt")).toUtf8();
    return path.constData();
}

MIRROR_API mirror_result_t mirror_start_mice_backend(
    const char *device_name,
    const mirror_gateway_callbacks_t *cbs,
    void *userdata)
{
    {
        QMutexLocker lock(&g_mutex);
        if (!g_initialized)
            return MIRROR_ERR_NOT_INITIALIZED;
        if (g_miceBackend)
            return MIRROR_OK;   // 已启动, 幂等
    }

    MiceBackend::Config cfg;
    cfg.deviceName = (device_name && *device_name)
                         ? QString::fromUtf8(device_name)
                         : QStringLiteral("MirrorCenter");

    mirror_gateway_callbacks_t cbsCopy{};
    if (cbs)
        cbsCopy = *cbs;
    {
        QMutexLocker lock(&g_mutex);
        g_gatewayCbs = cbsCopy;
        g_gatewayUserdata = userdata;
    }

    MiceBackend *backend = nullptr;
    invokeOnManager([&]() {
        auto *b = new MiceBackend(cfg);
        connectMiceSignals(b);
        if (!b->start()) {
            qWarning() << "[sdk] mice backend start failed";
            delete b;
            return;
        }
        backend = b;
    });
    if (!backend) {
        QMutexLocker lock(&g_mutex);
        g_gatewayCbs = mirror_gateway_callbacks_t{};
        g_gatewayUserdata = nullptr;
        setError("mice backend start failed");
        return MIRROR_ERR_SESSION_FAILED;
    }

    {
        QMutexLocker lock(&g_mutex);
        g_miceBackend = backend;
    }
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_stop_mice_backend(void)
{
    stopMiceAndCleanHandles();
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API void mirror_shutdown(void)
{
    QMutexLocker lock(&g_mutex);
    if (!g_initialized)
        return;

    // 先停网关(其实例会话句柄由网关持有, 需先回收)
    stopGatewayAndCleanHandles();
    // 再停 MS-MICE 接收端
    stopMiceAndCleanHandles();

    // 清理所有会话句柄(标记失效并停止)
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
        mirror_session_t *h = it.key();
        if (h->core) {
            h->core->stop();
        }
        h->valid = false;
        delete h;   // 释放宿主持有的句柄内存
    }
    g_sessions.clear();
    g_airplayPorts.clear();   // 释放所有 AirPlay 端口组

    if (g_manager) {
        g_manager->stopAll();
        g_manager->deleteLater();
        g_manager = nullptr;
    }

    cleanupInternalApp();
    g_initialized = false;
    setError("ok");
}

MIRROR_API mirror_result_t mirror_start_session(mirror_backend_t backend,
                                                const char *device_name,
                                                const char *exe_path,
                                                const char *args,
                                                const mirror_callbacks_t *cbs,
                                                void *userdata,
                                                mirror_session_t **out_session)
{
    if (!out_session)
        return MIRROR_ERR_INVALID_ARG;

    qInfo() << "[sdk] ENTER mirror_start_session backend=" << int(backend)
            << "device=" << (device_name ? device_name : "(null)");
    QMutexLocker lock(&g_mutex);
    qInfo() << "[sdk] mutex locked";
    if (!g_initialized)
        return MIRROR_ERR_NOT_INITIALIZED;
    qInfo() << "[sdk] g_initialized ok";

    *out_session = nullptr;
    auto *handle = new mirror_session_t();
    handle->valid = true;
    if (cbs)
        handle->cbs = *cbs;
    handle->userdata = userdata;

    registerHandle(handle);

    // 构建参数
    BackendType type = (backend == MIRROR_BACKEND_MIRACAST)
                           ? BackendType::Miracast : BackendType::AirPlay;

    QString name = device_name ? QString::fromUtf8(device_name) : QString();
    if (name.isEmpty())
        name = (type == BackendType::Miracast)
                   ? QStringLiteral("Miracast") : QStringLiteral("AirPlay");

    QString exe = exe_path ? QString::fromUtf8(exe_path) : QString();
    if (exe.isEmpty()) {
        if (type == BackendType::Miracast) {
#ifdef _WIN32
            // Windows: Miracast 接收由桌面辅助进程承担(无窗口,无 UWP 挂起限制)。
            // 随程序部署在 miracast-service/ 目录。
            const QString svc = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("miracast-service/MiracastReceiverService.exe"));
            if (QFile::exists(svc))
                exe = svc;
            else
                exe = g_manager->findBackendExe({QStringLiteral("MiracastReceiverService.exe"),
                                                 QStringLiteral("miracast-service/MiracastReceiverService.exe")});
#else
            exe = g_manager->findBackendExe({QStringLiteral("miracle-sinkctl")});
#endif
        } else {
            exe = g_manager->findBackendExe({QStringLiteral("uxplay.exe"),
                                             QStringLiteral("uxplay")});
        }
    }
    qInfo() << "[sdk] resolved exe=" << exe.toUtf8().constData();

    QStringList argList;
    if (args && *args) {
        argList = QString::fromUtf8(args).split(QLatin1Char(' '),
                                                Qt::SkipEmptyParts);
    }

    // AirPlay:把会话名作为 UxPlay 的广播设备名,方便手机端识别
    if (type == BackendType::AirPlay && !name.isEmpty()) {
        argList << QStringLiteral("-n") << name;
    }

    // AirPlay:默认用 d3d11videosink 输出。MirrorCenter 定制的 uxplay 靠
    // d3d11 窗口标题 "Direct3D11" 定位子窗口并输出 WINDOW_HANDLE 协议,
    // 只有指定该 sink 时窗口嵌入才生效;若调用方显式传了 -vs 则尊重其选择。
    if (type == BackendType::AirPlay && !argList.contains(QStringLiteral("-vs"))) {
        argList << QStringLiteral("-vs") << QStringLiteral("d3d11videosink");
    }

    // AirPlay 多实例:每会话独立 uxplay 进程,分配独立端口组 + 随机 MAC。
    // - 端口组(-p base):与其它会话错开,避免 TCP/UDP 端口冲突
    // - 随机 MAC(-m):不同实例使用不同 MAC,避免 AirPlay 配对/设备标识互相覆盖
    if (type == BackendType::AirPlay) {
        const int base = allocAirplayPortBase();
        handle->airplayPortBase = base;
        argList << QStringLiteral("-p") << QString::number(base)
                << QStringLiteral("-m");
    }

#ifdef _WIN32
    // Windows Miracast:分配空闲端口,Qt 侧监听,桌面接收服务出站连接发送帧。
    // 帧经 TCP 发送到 MirrorCenter,由 GStreamer appsrc→d3d11videosink 渲染。
    quint16 framePort = 0;
    if (type == BackendType::Miracast) {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        framePort = probe.serverPort();
        probe.close();

        argList << QStringLiteral("--port") << QString::number(framePort)
                << QStringLiteral("--name") << name;
    }
#endif

    // 在事件线程创建会话
    bool ok = false;
    CreateArg createArg{type, name, exe, argList};
    invokeOnManager([mgr = g_manager, createArg, handle, &ok]() {
        createSessionInThread(mgr, createArg, handle, &ok);
    });

    if (!ok || !handle->core) {
        unregisterHandle(handle);
        delete handle;
        setError("failed to create session");
        return MIRROR_ERR_SESSION_FAILED;
    }
    qInfo() << "[sdk] session created, id=" << handle->core->id().toUtf8().constData();

    // Miracast:启用 frame mode,TCP 端口由服务出站连接(FrameClient 在本地监听)
    if (type == BackendType::Miracast && framePort > 0) {
        handle->core->setFrameMode(QStringLiteral("127.0.0.1"), framePort);
        qInfo() << "[sdk] frameMode port=" << framePort;
    }

    // 启动子进程(swap chain 模式:服务输出 WINDOW_HANDLE 到 stdout)
    QPointer<MirrorSession> core = handle->core;
    qInfo() << "[sdk] invoking core->start()";
    invokeOnManager([core]() {
        if (core)
            core->start();
    });
    qInfo() << "[sdk] core->start() returned";

    *out_session = handle;
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_stop_session(mirror_session_t *session)
{
    if (!session)
        return MIRROR_ERR_INVALID_ARG;
    if (!isHandleValid(session))
        return MIRROR_ERR_NOT_FOUND;

    QPointer<MirrorSession> core = session->core;
    if (core) {
        invokeOnManager([core]() {
            if (core)
                core->stop();
        });
    }
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_start_miracast_group(
    int count,
    const char *device_name,
    const char *exe_path,
    mirror_session_t **out_sessions,
    int *out_count)
{
    if (!out_sessions || !out_count)
        return MIRROR_ERR_INVALID_ARG;
    if (count < 1 || count > 8)
        return MIRROR_ERR_INVALID_ARG;
    *out_count = 0;

    QMutexLocker lock(&g_mutex);
    if (!g_initialized)
        return MIRROR_ERR_NOT_INITIALIZED;

    QString name = device_name ? QString::fromUtf8(device_name) : QString();
    if (name.isEmpty())
        name = QStringLiteral("Miracast");

    // 后端 exe:仅主会话(0)承载服务进程,从会话(1..)为纯帧监听
    QString exe = exe_path ? QString::fromUtf8(exe_path) : QString();
    if (exe.isEmpty()) {
#ifdef _WIN32
        const QString svc = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("miracast-service/MiracastReceiverService.exe"));
        if (QFile::exists(svc))
            exe = svc;
        else
            exe = g_manager->findBackendExe({QStringLiteral("MiracastReceiverService.exe"),
                                             QStringLiteral("miracast-service/MiracastReceiverService.exe")});
#else
        exe = g_manager->findBackendExe({QStringLiteral("miracle-sinkctl")});
#endif
    }

    // 分配 count 个空闲帧端口(宿主每路一个 FrameClient 监听)
    QList<quint16> ports;
    for (int i = 0; i < count; ++i) {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            setError("no free frame port");
            return MIRROR_ERR_SESSION_FAILED;
        }
        ports.append(probe.serverPort());
        probe.close();
    }

    QStringList portStrs;
    for (quint16 p : ports)
        portStrs << QString::number(p);
    // 主会话:单服务进程承载全部连接, --ports p0,.. --max count
    QStringList argList;
    argList << QStringLiteral("--ports") << portStrs.join(QLatin1Char(','))
            << QStringLiteral("--max") << QString::number(count)
            << QStringLiteral("--name") << name;

    // 创建句柄
    QVector<mirror_session_t *> handles;
    handles.reserve(count);
    for (int i = 0; i < count; ++i) {
        auto *h = new mirror_session_t();
        h->valid = true;
        registerHandle(h);
        handles.append(h);
    }

    // 事件线程逐个创建 core
    bool allOk = true;
    for (int i = 0; i < count; ++i) {
        bool ok = false;
        CreateArg createArg;
        createArg.type = BackendType::Miracast;
        createArg.deviceName = (i == 0) ? name
                                        : QStringLiteral("%1-%2").arg(name).arg(i + 1);
        createArg.exePath = (i == 0) ? exe : QString();
        createArg.args = (i == 0) ? argList : QStringList();
        invokeOnManager([mgr = g_manager, createArg, handle = handles[i], &ok]() {
            createSessionInThread(mgr, createArg, handle, &ok);
        });
        if (!ok || !handles[i]->core) {
            allOk = false;
            break;
        }
    }

    if (!allOk) {
        for (mirror_session_t *h : handles) {
            if (h->core) {
                QPointer<MirrorSession> core = h->core;
                invokeOnManager([core]() {
                    if (core) {
                        core->stop();
                        core->deleteLater();
                    }
                });
                h->core = nullptr;
            }
            unregisterHandle(h);
            h->valid = false;
            delete h;
        }
        setError("failed to create miracast group");
        return MIRROR_ERR_SESSION_FAILED;
    }

    // 每路帧模式 + 启动(主会话启动服务进程,从会话走 external activation 纯监听)
    for (int i = 0; i < count; ++i)
        handles[i]->core->setFrameMode(QStringLiteral("127.0.0.1"), ports[i]);
    for (int i = 0; i < count; ++i) {
        QPointer<MirrorSession> core = handles[i]->core;
        invokeOnManager([core]() {
            if (core)
                core->start();
        });
    }

    for (int i = 0; i < count; ++i)
        out_sessions[i] = handles[i];
    *out_count = count;
    setError("ok");
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_stop_all(void)
{
    if (!g_initialized)
        return MIRROR_ERR_NOT_INITIALIZED;

    invokeOnManager([]() {
        if (g_manager)
            g_manager->stopAll();
    });
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_destroy_session(mirror_session_t *session)
{
    if (!session)
        return MIRROR_ERR_INVALID_ARG;
    if (!isHandleValid(session))
        return MIRROR_ERR_NOT_FOUND;

    QPointer<MirrorSession> core = session->core;
    if (core) {
        invokeOnManager([core]() {
            if (core)
                core->stop();
        });
    }

    unregisterHandle(session);
    session->valid = false;
    if (session->airplayPortBase != 0) {
        freeAirplayPortBase(session->airplayPortBase);
        session->airplayPortBase = 0;
    }
    delete session;
    return MIRROR_OK;
}

MIRROR_API uint64_t mirror_get_window(mirror_session_t *session)
{
    if (!session || !isHandleValid(session) || !session->core)
        return 0;
    return static_cast<uint64_t>(session->core->windowHandle());
}

MIRROR_API uint64_t mirror_get_process_id(mirror_session_t *session)
{
    if (!session || !isHandleValid(session) || !session->core)
        return 0;
    return static_cast<uint64_t>(session->core->processId());
}

MIRROR_API mirror_state_t mirror_get_state(mirror_session_t *session)
{
    if (!session || !isHandleValid(session))
        return MIRROR_STATE_CLOSED;
    if (!session->core) {
        // MS-MICE 等无后端子进程的句柄: 会话存活即视为运行中
        return MIRROR_STATE_RUNNING;
    }

    switch (session->core->state()) {
    case SessionState::Starting:   return MIRROR_STATE_STARTING;
    case SessionState::Running:    return MIRROR_STATE_RUNNING;
    case SessionState::WindowReady: return MIRROR_STATE_WINDOW_READY;
    case SessionState::Failed:     return MIRROR_STATE_FAILED;
    default:                       return MIRROR_STATE_CLOSED;
    }
}

MIRROR_API const char *mirror_get_device_name(mirror_session_t *session)
{
    if (!session || !isHandleValid(session) || !session->core)
        return nullptr;
    static thread_local QString nameBuf;
    nameBuf = session->core->deviceName();
    return nameBuf.toUtf8().constData();
}

MIRROR_API mirror_result_t mirror_get_frame(mirror_session_t *session,
                                            mirror_frame_t *frame)
{
    if (!session || !frame || !isHandleValid(session))
        return MIRROR_ERR_INVALID_ARG;

    QImage img;
    if (session->core) {
        img = session->core->latestFrame();
    } else {
        // MS-MICE 等无子进程后端的句柄:帧由事件线程写入 frameCache
        QMutexLocker lock(&g_mutex);
        img = session->frameCache;
    }
    if (img.isNull())
        return MIRROR_ERR_NOT_FOUND;

    QMutexLocker lock(&g_mutex);
    session->frameCache = img;   // 持有副本,保证 data 指针有效
    frame->width  = session->frameCache.width();
    frame->height = session->frameCache.height();
    frame->stride = session->frameCache.bytesPerLine();
    frame->data   = session->frameCache.constBits();
    return MIRROR_OK;
}

MIRROR_API mirror_result_t mirror_set_callbacks(mirror_session_t *session,
                                                const mirror_callbacks_t *cbs,
                                                void *userdata)
{
    if (!session || !isHandleValid(session))
        return MIRROR_ERR_NOT_FOUND;
    if (cbs)
        session->cbs = *cbs;
    else
        session->cbs = mirror_callbacks_t{};
    session->userdata = userdata;
    return MIRROR_OK;
}

MIRROR_API const char *mirror_last_error(void)
{
    return g_lastError;
}

} // extern "C"
