#pragma once

#include <QWidget>
#include <QList>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QPixmap>

#include "mirror_api.h"

class SessionView;
class QGridLayout;
class QVBoxLayout;
class QLabel;
class QFrame;

/** 投屏来源(供控制台列表展示) */
struct SourceItem {
    QString sessionId;       // 会话标识(选中置顶/缩略图定位用)
    QString name;            // 来源名称(手机名或 IP)
    QString ip;              // 客户端 IP
    QString status;          // 状态文本(投屏中/连接中)
    mirror_backend_t backend = MIRROR_BACKEND_AIRPLAY;
};

/**
 * 桌面窗口(主窗口,全屏承载区)
 * - 黑色背景,显示所有投屏会话
 * - 支持 F11 / 双击 切换全屏
 * - 没有任何装饰菜单,菜单由 ControlPanel 承载
 * - 底部"显示/隐藏控制台"小按钮(控制台隐藏时可见)
 */
class DesktopWindow : public QWidget
{
    Q_OBJECT
public:
    explicit DesktopWindow(QWidget *parent = nullptr);
    ~DesktopWindow() override;

    /** 添加会话 */
    SessionView *addSession(const QString &name, mirror_backend_t backend);

    /** 当前布局模式 1/2/3/4/6 */
    int  layoutMode() const { return m_layoutMode; }
    void setLayoutMode(int mode);

    /** 会话数量 */
    int  sessionCount() const { return m_views.size(); }

    /** 当前投屏来源列表(控制台展示用) */
    QList<SourceItem> sourceItems() const;

    /** 取指定会话的最新缩略图(控制台列表用) */
    QPixmap thumbnailFor(const QString &sessionId) const;

    /** 解码能力:软解(avdec_*) 时最多 4 格;硬解(d3d11/nv/vaapi 等)最多 16 格 */
    int  maxGrid() const;

public slots:
    void startAirPlay();
    void startMiracast();
    void toggleFullscreen();
    void onSessionClosed(const QString &sessionId);
    /** 控制台选中某来源 → 把它排到主窗口首位 */
    void focusSession(const QString &sessionId);

private:
    void buildUi();
    void relayout();
    void updateEmptyState();
    /** 视图点击全屏/还原 → 切换该会话独占全屏 */
    void onViewFullscreen(const QString &sessionId);

protected:
    void resizeEvent(QResizeEvent *e) override;

    /** 网关设备连入 → 创建会话视图 */
    void onGatewayClientConnected(mirror_session_t *session, const QString &ip);
    /** 网关设备断开 → 移除会话视图 */
    void onGatewayClientDisconnected(mirror_session_t *session);

    // 网关回调(C 函数指针, SDK 事件线程触发)
    static void gatewayLogCallback(const char *message, void *userdata);
    static void gatewayClientConnectedCallback(mirror_session_t *session,
                                               const char *client_ip,
                                               void *userdata);
    static void gatewayClientDisconnectedCallback(mirror_session_t *session,
                                                  const char *client_ip,
                                                  void *userdata);
    static void gatewayClientInfoCallback(mirror_session_t *session,
                                          const char *client_name,
                                          const char *client_model,
                                          void *userdata);
    static void gatewayDecoderCallback(mirror_session_t *session,
                                       const char *decoder,
                                       void *userdata);

    QWidget        *m_canvasInner   = nullptr;
    QVBoxLayout    *m_canvasLayout  = nullptr;
    QFrame         *m_emptyCard     = nullptr;
    QLabel         *m_emptyLabel    = nullptr;
    QGridLayout    *m_grid          = nullptr;
    QToolButton    *m_toggleCtrlBtn = nullptr;  // "显示控制台"小按钮
    QList<SessionView *> m_views;
    SessionView *m_focusView = nullptr;   // 独占全屏的会话视图(非空时只显示它)
    int  m_layoutMode = 0;   // 0=按会话数自动(1全屏/2左右/3+四宫格), 1/2/3/4/6=手动覆盖
    bool m_gatewayStarted  = false;   // AirPlay 网关已启动(幂等)
    bool m_miracastStarted = false;
    QString m_decoder;                 // 实例上报的实际视频解码器(空=未知)
    int  m_maxGrid = 16;               // 根据解码器限定的最大格数(软解4/硬解16)

    // 上次布局状态(快速路径:无变化则跳过重排, 避免切换闪烁)
    QList<SessionView *> m_lastShown;
    int  m_lastCols = -1;
    int  m_lastRows = -1;
    bool m_lastFullBleed = false;
    bool m_lastEmpty = true;

    // 网关会话映射:SDK 句柄 → 视图(回调线程访问, 加锁)
    QMutex m_gatewayMutex;
    QHash<mirror_session_t *, SessionView *> m_gatewayViews;

public:
    /** 供 ControlPanel 调用:显示/隐藏"显示控制台"小按钮 */
    void showToggleCtrlBtn(bool show);
    QToolButton *toggleCtrlButton() const { return m_toggleCtrlBtn; }

signals:
    void sessionCountChanged(int n);
    void statusMessage(const QString &msg);
    /** 投屏来源列表变化(控制台据此刷新列表) */
    void sourcesChanged();
    /** 用户点击了"显示控制台"小按钮(由 ControlPanel 监听) */
    void requestShowControlPanel();
};
