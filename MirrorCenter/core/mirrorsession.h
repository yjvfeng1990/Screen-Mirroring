#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QPointer>
#include <QProcess>
#include <QImage>

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

    /// Frame mode: enable FrameClient connecting to given address/port
    void setFrameMode(const QString &address, quint16 tcpPort);

    /// Latest frame (frame mode)
    QImage latestFrame() const;

    /// Video size (frame mode)
    QSize videoSize() const;

    void start();
    void stop();

signals:
    void stateChanged(const QString &id, mirror::SessionState state);
    void windowReady(const QString &id, qulonglong handle);
    void logMessage(const QString &id, const QString &message);
    void frameReady(const QString &id);

private slots:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);
    void onFrameClientReady();
    void onFrameClientDisconnected();

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
};

} // namespace mirror
