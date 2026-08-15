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
class QShowEvent;
class QHideEvent;
class QCloseEvent;
class QResizeEvent;
class QMoveEvent;
class QEvent;

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

protected:
    // 启动闪烁诊断: 记录 show/hide 事件序列
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    // HWND 重建诊断: createWinId 后若 winId 变化说明原生窗口被销毁重建
    bool event(QEvent *e) override;
    // 主窗口移动/跨屏 → 联动各视图的信息栏重定位(信息栏是独立顶层窗)
    void moveEvent(QMoveEvent *e) override;
    // 主窗口关闭 → 通知 main 联动关闭控制面板
    void closeEvent(QCloseEvent *e) override;
    // 最小化/还原 → 联动隐藏/还原控制面板与触发条
    void changeEvent(QEvent *e) override;

public:
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
    void startMiceBackend();
    /**
     * 预创建 Miracast 占位视图(4 路槽位, 不启动服务)。
     * 必须在窗口显示前调用: QOpenGLWidget 在已显示窗口上动态创建会触发
     * 父窗口 HWND 重建(Qt6 行为), 表现为"窗口打开后又消失重显"。
     * 视图先在离屏预渲染阶段就绪, startMiracast 仅接管 SDK 句柄。
     */
    void createMiracastPlaceholders();
    void toggleFullscreen();
    void onSessionClosed(const QString &sessionId);
    /** 控制台"移除投屏设备":网关会话先断开设备再清理视图 */
    void removeSession(const QString &sessionId);
    /** 控制台选中某来源 → 把它排到主窗口首位 */
    void focusSession(const QString &sessionId);
    /** 控制面板展开时隐藏右缘触发条(独立顶层窗会浮在面板之上), 收起后恢复 */
    void setSideTriggerVisible(bool visible);

private:
    void buildUi();
    void relayout();
    void updateEmptyState();
    /** 视图点击全屏/还原 → 切换该会话独占全屏 */
    void onViewFullscreen(const QString &sessionId);
    /** 右缘触发条重定位(独立顶层窗: 屏幕全局坐标 + 显示控制) */
    void updateSideTriggerPos();

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
    QWidget        *m_sideTrigger   = nullptr;  // 主窗口右缘内侧触发条(替代原悬浮条)
    bool            m_sideTriggerEnabled = true; // 控制面板展开时禁用(避免浮在面板上)
    QList<SessionView *> m_views;
    SessionView *m_focusView = nullptr;   // 独占全屏的会话视图(非空时只显示它)
    int  m_layoutMode = 0;   // 0=按会话数自动(1全屏/2左右/3+四宫格), 1/2/3/4/6=手动覆盖
    bool m_gatewayStarted  = false;   // AirPlay 网关已启动(幂等)
    bool m_miracastStarted = false;
    bool m_miceStarted     = false;   // MS-MICE 接收端已启动(幂等)
    QString m_decoder;                 // 实例上报的实际视频解码器(空=未知)
    int  m_maxGrid = 16;               // 根据解码器限定的最大格数(软解4/硬解16)
    WId  m_lastWinId = 0;              // 上次 winId(诊断 HWND 重建)

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

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void sessionCountChanged(int n);
    void statusMessage(const QString &msg);
    /** 投屏来源列表变化(控制台据此刷新列表) */
    void sourcesChanged();
    /** 用户点击了"显示控制台"小按钮(由 ControlPanel 监听) */
    void requestShowControlPanel();
    /** 主窗口关闭请求(由 main 联动关闭控制面板) */
    void closeRequested();
    /** 主窗口最小化状态变化(由 main 联动隐藏/还原控制面板) */
    void windowMinimizedChanged(bool minimized);
    /** 主窗口右缘触发条被悬停/点击(由 main 展开控制面板) */
    void sideTriggerActivated();
};
