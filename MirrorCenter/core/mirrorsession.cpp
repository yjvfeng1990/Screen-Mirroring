#include "mirrorsession.h"
#include "frameclient.h"
#include "mirror_api.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QDebug>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mirror {

namespace {
// Windows Job Object:把后端子进程绑定到 MirrorCenter 进程,
// 父进程无论正常退出还是被强杀(Stop-Process -Force), 子进程都会自动终止。
// 否则强杀 MirrorCenter 会残留孤儿 uxplay/MiracastReceiverService,
// 多个孤儿实例抢同一端口并同时广播同名 mDNS, 导致 AirPlay 搜索不到广播。

#ifdef _WIN32
HANDLE backendJob()
{
    static HANDLE job = []() {
        HANDLE j = ::CreateJobObjectW(nullptr, nullptr);
        if (!j)
            return j;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return j;
    }();
    return job;
}

void attachBackendToJob(QProcess *proc)
{
    HANDLE job = backendJob();
    if (!job || !proc)
        return;
    const DWORD pid = DWORD(proc->processId());
    if (!pid)
        return;
    HANDLE hProc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
    if (!hProc)
        return;
    ::AssignProcessToJobObject(job, hProc);
    ::CloseHandle(hProc);
}
#endif
} // namespace

MirrorSession::MirrorSession(const QString &id,
                             BackendType type,
                             const QString &deviceName,
                             const QString &backendExe,
                             const QStringList &args,
                             QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_type(type)
    , m_deviceName(deviceName)
    , m_backendExe(backendExe)
    , m_backendArgs(args)
{
}

MirrorSession::~MirrorSession()
{
    stop();
}

void MirrorSession::setFrameMode(const QString &address, quint16 tcpPort)
{
    m_frameMode = true;
    m_frameAddress = address;
    m_framePort = tcpPort;
}

QImage MirrorSession::latestFrame() const
{
    return m_frameClient ? m_frameClient->latestFrame() : QImage();
}

QSize MirrorSession::videoSize() const
{
    return m_frameClient ? m_frameClient->videoSize() : QSize();
}

void MirrorSession::setFrameTargetFps(int fps)
{
    if (m_frameClient)
        m_frameClient->setTargetFps(fps);
}

void MirrorSession::setFrameTargetEdge(int edge)
{
    if (m_frameClient)
        m_frameClient->setTargetEdge(edge);
}

void MirrorSession::start()
{
    if (m_process)
        return;

    // Windows Miracast:接收由桌面辅助进程(MiracastReceiverService.exe)承担,
    // 无窗口、无 UWP 挂起限制。子进程作为普通 QProcess 启动(见下方流程)。
    if (m_backendExe.isEmpty()) {
        if (m_frameMode) {
            m_frameClient = new FrameClient(this);
            connect(m_frameClient, &FrameClient::connected,
                    this, &MirrorSession::onFrameClientReady);
            connect(m_frameClient, &FrameClient::disconnected,
                    this, &MirrorSession::onFrameClientDisconnected);
            connect(m_frameClient, &FrameClient::frameReady,
                    this, [this]() { emit frameReady(m_id); });
            // UWP 出站连接本机监听端口(AppContainer 入站隔离,方向必须反转)
            m_frameClient->startListening(m_framePort);
        }
        setState(SessionState::Starting);
        emit logMessage(m_id, QStringLiteral("external activation, frame mode listen port=%1")
                                .arg(m_framePort));
        return;
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef _WIN32
    // 隐藏子进程控制台窗口(uxplay / miracast-service 是控制台程序, 默认会弹 conhost 窗口)
    m_process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif

    // AirPlay(UxPlay)依赖 GStreamer,Windows 下插件随 exe 部署在 gstreamer-1.0/ 目录,
    // 必须显式指定 GST_PLUGIN_PATH,否则 uxplay 报 "Required gstreamer plugin not found"
    if (m_type == BackendType::AirPlay) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString pluginDir =
            QCoreApplication::applicationDirPath() + QStringLiteral("/gstreamer-1.0");
        QStringList paths;
        if (env.contains(QStringLiteral("GST_PLUGIN_PATH")))
            paths << env.value(QStringLiteral("GST_PLUGIN_PATH"));
        paths << pluginDir;
        env.insert(QStringLiteral("GST_PLUGIN_PATH"), paths.join(QLatin1Char(';')));
        // 2 分屏铺满控制文件:uxplay 轮询该文件, app 侧写 "1"/"0" 切换
        env.insert(QStringLiteral("UXPLAY_FILL_FILE"),
                   QString::fromUtf8(mirror_airplay_fill_file()));
        m_process->setProcessEnvironment(env);
        qInfo() << "[core] GST_PLUGIN_PATH=" << paths.join(QLatin1Char(';')).toUtf8().constData();
    }
    connect(m_process, &QProcess::readyReadStandardOutput, this, &MirrorSession::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError,  this, &MirrorSession::onReadyReadStderr);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MirrorSession::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &MirrorSession::onErrorOccurred);

    setState(SessionState::Starting);
    emit logMessage(m_id, QStringLiteral("start: %1 %2").arg(m_backendExe, m_backendArgs.join(' ')));
    qInfo() << "[core] start() backendExe=" << m_backendExe.toUtf8().constData()
            << "args=" << m_backendArgs.join(' ').toUtf8().constData();
    m_process->start(m_backendExe, m_backendArgs);
    qInfo() << "[core] QProcess::start() returned, state=" << int(m_process->state());

#ifdef _WIN32
    // 绑定到 Job Object:父进程(含被强杀)退出时子进程自动终止, 防孤儿堆积
    attachBackendToJob(m_process);
#endif

    // Frame mode: connect to the receiver's TCP frame server.
    if (m_frameMode) {
        m_frameClient = new FrameClient(this);
        connect(m_frameClient, &FrameClient::connected, this, &MirrorSession::onFrameClientReady);
        connect(m_frameClient, &FrameClient::disconnected, this, &MirrorSession::onFrameClientDisconnected);
        connect(m_frameClient, &FrameClient::frameReady,
                this, [this]() { emit frameReady(m_id); });
        m_frameClient->startListening(m_framePort);
    }
}

void MirrorSession::stop()
{
    if (m_frameClient) {
        m_frameClient->stopListening();
        m_frameClient->deleteLater();
        m_frameClient = nullptr;
    }
    if (!m_process)
        return;
    m_stopping = true;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    m_process->deleteLater();
    m_process = nullptr;
    setState(SessionState::Closed);
}

void MirrorSession::onReadyReadStdout()
{
    if (!m_process)
        return;
    while (m_process->canReadLine()) {
        const QByteArray line = m_process->readLine().trimmed();
        if (line.isEmpty())
            continue;
        emit logMessage(m_id, QString::fromUtf8(line));

        // Protocol: after startup the child outputs WINDOW_HANDLE=<decimal>
        static const QRegularExpression re(QStringLiteral("WINDOW_HANDLE=(\\d+)"));
        const auto match = re.match(QString::fromUtf8(line));
        if (match.hasMatch()) {
            m_windowHandle = match.captured(1).toULongLong();
            setState(SessionState::WindowReady);
            emit windowReady(m_id, m_windowHandle);
        }
    }
}

void MirrorSession::onReadyReadStderr()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardError();
    if (!data.isEmpty())
        emit logMessage(m_id, QStringLiteral("[stderr] %1").arg(QString::fromUtf8(data).trimmed()));
}

void MirrorSession::onFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(exitCode)
    Q_UNUSED(status)
    if (m_stopping) {
        setState(SessionState::Closed);
    } else {
        emit logMessage(m_id, QStringLiteral("child exited exit=%1").arg(exitCode));
        setState(SessionState::Closed);
    }
    m_windowHandle = 0;
}

void MirrorSession::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        emit logMessage(m_id, QStringLiteral("failed to start: %1").arg(m_backendExe));
        setState(SessionState::Failed);
    }
}

void MirrorSession::onFrameClientReady()
{
    emit logMessage(m_id, QStringLiteral("frame link established"));
    setState(SessionState::WindowReady);
}

void MirrorSession::onFrameClientDisconnected()
{
    emit logMessage(m_id, QStringLiteral("frame link lost"));
    // 帧链路断开(投屏设备退出):回到"等待设备"状态而不是关闭会话。
    // Miracast 多路服务进程共享, 槽位保持监听, 新设备连入同一端口即可重连
    // (FrameClient 的 m_server 未关闭)。UI 收到 Starting 状态后清空画面/列表。
    setState(SessionState::Starting);
}

void MirrorSession::setState(SessionState s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(m_id, s);
}

} // namespace mirror
