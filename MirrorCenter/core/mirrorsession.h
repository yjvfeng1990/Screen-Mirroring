#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QPointer>
#include <QProcess>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>

class QProcess;

namespace mirror {

class FrameClient;

enum class BackendType {
    AirPlay,    // UxPlay
    Miracast,   // UWP helper (Windows) or miracle-sinkctl (Linux)
};

enum class SessionState {
    Starting,    // child process starting
    Running,     // child process running
    WindowReady, // child window handle available
    Failed,      // failed to start
    Closed,      // closed
};

/**
 * A single cast session (no UI, usable by any host).
 * Two operating modes:
 *   - Window mode (AirPlay): start child process, parse WINDOW_HANDLE protocol
 *     from stdout, expose the child window handle.
 *   - Frame mode (Miracast): start the UWP receiver, FrameClient connects via
 *     TCP and receives frames; latest frame is exposed.
 */
class MirrorSession : public QObject
{
    Q_OBJECT
public:
    explicit MirrorSession(const QString &id,
                           BackendType type,
                           const QString &deviceName,
                           const QString &backendExe,
                           const QStringList &args,
                           QObject *parent = nullptr);
    ~MirrorSession() override;

    QString id() const { return m_id; }
    BackendType backendType() const { return m_type; }
    QString deviceName() const { return m_deviceName; }
    SessionState state() const { return m_state; }

    /// Child window handle: HWND on Windows, X11 Window ID on Linux (0 = not ready)
    qulonglong windowHandle() const { return m_windowHandle; }

    /// Child process id (0 = not started yet)
    qint64 processId() const { return m_process ? m_process->processId() : 0; }

    /// Frame mode: enable FrameClient connecting to given address/port
    void setFrameMode(const QString &address, quint16 tcpPort);

    /// Latest frame (frame mode)
    QImage latestFrame() const;

    /// Video size (frame mode)
    QSize videoSize() const;

    /// 全屏放大: 向接收服务设置该路目标帧率(0 恢复默认, 1 等低频)
    void setFrameTargetFps(int fps);

    /// 混合路数分档: 向接收服务设置该路读回最大边(0 不缩放, >0 上限)
    void setFrameTargetEdge(int edge);

    /// 全屏放大场景: 本路静音/取消静音(Miracast 组按连接走 SETMUTE)。
    void setTargetMute(bool mute);

    /// 移除投屏源: 请求断开本路连接(Miracast 按连接走 SETDISC, 服务进程保留)。
    void setTargetDisconnect();

    void start();
    void stop();

signals:
    void stateChanged(const QString &id, mirror::SessionState state);
    void windowReady(const QString &id, qulonglong handle);
    void logMessage(const QString &id, const QString &message);
    void frameReady(const QString &id);
    /// 投屏设备真实名称(服务端经帧通道上报, 如 "Honor 10")
    void clientInfoChanged(const QString &id, const QString &name, const QString &model);

private slots:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);
    void onFrameClientReady();
    void onFrameClientDisconnected();
    void onFrameIdleTimeout();

private:
    void setState(SessionState s);

    QString m_id;
    BackendType m_type;
    QString m_deviceName;
    QString m_backendExe;
    QStringList m_backendArgs;

    QPointer<QProcess> m_process;
    SessionState m_state = SessionState::Starting;
    qulonglong m_windowHandle = 0;
    bool m_stopping = false;

    // Frame mode
    QPointer<FrameClient> m_frameClient;
    bool m_frameMode = false;
    QString m_frameAddress;
    quint16 m_framePort = 0;

    // 帧空闲检测:投屏源断开但帧通道未及时关闭时(服务端 Disconnected 事件
    // 可能延迟/不触发), 连续 3s 无帧 → 视设备已断开 → 清画面回等待。
    QTimer m_frameIdleTimer;
    QElapsedTimer m_frameTimer;
};

} // namespace mirror
