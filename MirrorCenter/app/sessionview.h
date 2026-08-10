#pragma once

#include <QWidget>
#include <QPointer>
#include <QProcess>
#include <QImage>
#include <QTimer>
#include <QPixmap>

class QLabel;
class QWindow;

// 引入 SDK C 接口
#include "mirror_api.h"

/**
 * 单个投屏会话视图(SDK 演示宿主)。
 * Miracast:通过 SDK 启动 UWP 接收进程 → 帧回调中取帧 → 绘制到 QLabel(自建会话)。
 * AirPlay 网关:视图由网关回调提供的 SDK 句柄包装而来(adoptGatewaySession),
 *            不拥有句柄生命周期, 设备断开时句柄由 SDK 回收, 视图仅解除嵌入。
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
    QString clientIp() const { return m_clientIp; }
    bool isRunning() const { return m_running; }
    mirror_backend_t backend() const { return m_backend; }
    bool isGatewayMode() const { return m_gatewayMode; }

    /** 网关模式:包装 SDK 已建好的会话句柄(由 on_client_connected 回调提供) */
    void adoptGatewaySession(mirror_session_t *sdkSession, const QString &clientIp);

    void stop();
    /** 设备断开:移除窗口嵌入(视图随后由 DesktopWindow 删除) */
    void detach();
    /** 由 DesktopWindow 通知当前是否独占全屏(切换按钮图标) */
    void setFullscreenActive(bool active);
    /** 更新来源手机名称/型号(显示在悬浮标签) */
    void setClientInfo(const QString &name, const QString &model);
    /** 当前画面缩略图(控制台列表用);无画面返回空 */
    QPixmap thumbnail() const;

signals:
    void sessionClosed(const QString &sessionId);
    void statusChanged(const QString &sessionId, const QString &status);
    /** 用户点击了全屏/还原按钮(DesktopWindow 负责切换该会话独占全屏) */
    void fullscreenRequested(const QString &sessionId);
    /** 缩略图刷新(控制台列表据此更新) */
    void thumbnailUpdated();

protected:
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void buildUi();
    /** 自建会话(Miracast / 独立 AirPlay):通过 mirror_start_session 启动后端 */
    void startStandaloneSession();
    /** 把本视图回调挂到 SDK 会话上 */
    void attachCallbacks();
    void attachWindow(qulonglong wid);
    void setStatus(const QString &s);
    void renderFrame();
    /** 抓取当前画面(平台相关实现) */
    QImage captureThumbnail();
    /** 画面内容指纹比较:有变化返回 true(缩略图去重, 无变化不切图) */
    bool thumbChanged(const QImage &thumb);
    /** 信息标签为独立顶层小窗, 跟随本视图左上角悬浮(避免被 D3D11 原生窗口遮挡) */
    void updateInfoBadge();
    /** 静音/取消静音(按会话后端子进程 PID 控制音频会话) */
    void toggleMute();
    /** 全屏/还原 */
    void toggleFullscreen();

    // SDK 回调(C 函数指针)
    static void onStateCallback(mirror_session_t *session, mirror_state_t state, void *userdata);
    static void onWindowCallback(mirror_session_t *session, uint64_t handle, void *userdata);
    static void onLogCallback(mirror_session_t *session, const char *message, void *userdata);
    static void onFrameCallback(mirror_session_t *session, void *userdata);

    QString m_sessionId;
    QString m_deviceName;
    QString m_clientIp;              // 网关模式:已连接设备 IP
    mirror_backend_t m_backend;
    mirror_session_t *m_sdkSession = nullptr;
    bool m_gatewayMode = false;      // 句柄由网关提供, stop() 不销毁

    QPointer<QWindow> m_childWindow;
    QWidget *m_videoArea      = nullptr;   // 视频区(占位/嵌入容器/Miracast 帧)
    QWidget *m_infoBadge      = nullptr;   // 悬浮信息标签(独立顶层窗, 设备名 + 状态)
    QLabel  *m_statusDot      = nullptr;   // 状态指示点
    QLabel  *m_statusLabel    = nullptr;   // 状态文字
    QLabel  *m_devLabel       = nullptr;   // 来源手机名称(初始为 IP)
    QLabel  *m_videoLabel     = nullptr;   // Miracast 帧显示
    class QToolButton *m_muteBtn = nullptr; // 静音切换
    class QToolButton *m_fullBtn = nullptr; // 全屏切换
    bool m_muted            = false;
    bool m_running          = false;
    QTimer m_thumbTimer;                 // 缩略图抓取节拍(内容变化时 1.2s, 静止时 3s)
    QPixmap m_lastThumb;                 // 最近一帧缩略图
    QImage m_thumbFp;                    // 上次画面指纹(16x9, 用于变化检测)
    bool m_hasThumbFp = false;
};
