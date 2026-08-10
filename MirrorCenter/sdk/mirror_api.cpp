#include "mirror_api.h"
#include "sessionmanager.h"
#include "mirrorsession.h"

#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QThread>
#include <QMutex>
#include <QMetaObject>
#include <QTimer>
#include <QTcpServer>
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
};

// 递归锁:mirror_start_session 等会持锁调用 registerHandle/isHandleValid 等辅助函数,
// 若用普通 QMutex 会在同线程重入时自死锁(表现为点击投屏后 UI 卡死)
static QRecursiveMutex g_mutex;
static SessionManager *g_manager = nullptr;   // 生命周期:随 SDK 事件线程,不跨线程删除
static QHash<mirror_session_t *, mirror_session_t *> g_sessions; // 指针登记表(防野指针)
static QThread *g_thread = nullptr;
static bool g_initialized = false;

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

MIRROR_API void mirror_shutdown(void)
{
    QMutexLocker lock(&g_mutex);
    if (!g_initialized)
        return;

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
            // Windows: Miracast 接收由 UWP 辅助进程承担,通过 shell:AppsFolder 启动
            exe = QStringLiteral("explorer.exe");
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

#ifdef _WIN32
    // Windows Miracast 帧模式:分配空闲端口,通过 miracast:// 协议激活 UWP 接收进程,
    // Qt 侧连该端口收帧。不用 QProcess/explorer 启动 —— explorer 会把协议 URL 当作
    // 本地路径解析(失败则回退打开"文档"文件夹),必须用 ShellExecuteW 触发协议激活。
    quint16 framePort = 0;
    if (type == BackendType::Miracast) {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        framePort = probe.serverPort();
        probe.close();

        // 外部激活模式:无子进程,由 ShellExecuteW 在下方激活
        exe.clear();
        argList.clear();
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

#ifdef _WIN32
    // 帧模式:通过 miracast:// 协议激活 UWP 接收进程(带端口/名称参数),
    // Qt 侧随后连接其 TCP 帧服务器。
    if (type == BackendType::Miracast && framePort != 0) {
        QPointer<MirrorSession> core = handle->core;
        invokeOnManager([core, framePort]() {
            if (core)
                core->setFrameMode(QStringLiteral("127.0.0.1"), framePort);
        });

        const QString url = QStringLiteral("miracast://receive?port=%1&name=%2")
                                .arg(framePort).arg(name);
        qInfo() << "[sdk] ShellExecute protocol activation:" << url.toUtf8().constData();
        const HINSTANCE r = ShellExecuteW(nullptr, L"open",
                                          reinterpret_cast<LPCWSTR>(url.utf16()),
                                          nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            qWarning() << "[sdk] ShellExecuteW failed, err="
                       << reinterpret_cast<INT_PTR>(r);
        }
    }
#endif

    // 启动子进程
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
    delete session;
    return MIRROR_OK;
}

MIRROR_API uint64_t mirror_get_window(mirror_session_t *session)
{
    if (!session || !isHandleValid(session) || !session->core)
        return 0;
    return static_cast<uint64_t>(session->core->windowHandle());
}

MIRROR_API mirror_state_t mirror_get_state(mirror_session_t *session)
{
    if (!session || !isHandleValid(session) || !session->core)
        return MIRROR_STATE_CLOSED;

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
    if (!session || !frame || !isHandleValid(session) || !session->core)
        return MIRROR_ERR_INVALID_ARG;

    const QImage img = session->core->latestFrame();
    if (img.isNull())
        return MIRROR_ERR_NOT_FOUND;

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
