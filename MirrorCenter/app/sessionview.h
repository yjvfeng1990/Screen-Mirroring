#pragma once

#include <QWidget>
#include <QPointer>
#include <QProcess>
#include <QImage>

class QLabel;
class QWindow;

// 引入 SDK C 接口
#include "mirror_api.h"

/**
 * 单个投屏会话视图(SDK 演示宿主)。
 * AirPlay:通过 SDK 启动子进程 → 拿到窗口句柄 → 嵌入本视图。
 * Miracast:通过 SDK 启动 UWP 接收进程 → 帧回调中取帧 → 绘制到 QLabel。
 */
class SessionView : public QWidget
{
    Q_OBJECT
public:
    explicit SessionView(const QString &deviceName,
                         mirror_backend_t backend,
                         QWidget *parent = nullptr);
    ~SessionView() override;

    QString deviceName() const { return m_deviceName; }
    QString sessionId() const { return m_sessionId; }
    bool isRunning() const { return m_running; }

    void stop();

signals:
    void sessionClosed(const QString &sessionId);
    void statusChanged(const QString &sessionId, const QString &status);

private:
    void attachWindow(qulonglong wid);
    void setStatus(const QString &s);
    void renderFrame();

    // SDK 回调(C 函数指针)
    static void onStateCallback(mirror_session_t *session, mirror_state_t state, void *userdata);
    static void onWindowCallback(mirror_session_t *session, uint64_t handle, void *userdata);
    static void onLogCallback(mirror_session_t *session, const char *message, void *userdata);
    static void onFrameCallback(mirror_session_t *session, void *userdata);

    QString m_sessionId;
    QString m_deviceName;
    mirror_backend_t m_backend;
    mirror_session_t *m_sdkSession = nullptr;

    QPointer<QWindow> m_childWindow;
    QWidget *m_topBar         = nullptr;   // 顶栏容器
    QLabel  *m_statusDot      = nullptr;   // 状态指示点
    QLabel  *m_statusLabel    = nullptr;   // 状态文字
    QWidget *m_placeholder    = nullptr;   // 占位容器
    QLabel  *m_videoLabel     = nullptr;   // Miracast 帧显示
    bool m_running            = false;
};
