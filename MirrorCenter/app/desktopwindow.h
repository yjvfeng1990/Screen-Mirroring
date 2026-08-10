#pragma once

#include <QWidget>
#include <QList>
#include <QString>

#include "mirror_api.h"

class SessionView;
class QGridLayout;
class QLabel;
class QFrame;

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

public slots:
    void startAirPlay();
    void startMiracast();
    void toggleFullscreen();
    void onSessionClosed(const QString &sessionId);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void buildUi();
    void relayout();
    void updateEmptyState();

    QWidget        *m_canvasInner   = nullptr;
    QFrame         *m_emptyCard     = nullptr;
    QLabel         *m_emptyLabel    = nullptr;
    QGridLayout    *m_grid          = nullptr;
    QToolButton    *m_toggleCtrlBtn = nullptr;  // "显示控制台"小按钮
    QList<SessionView *> m_views;
    int  m_layoutMode = 1;
    bool m_airplayStarted  = false;
    bool m_miracastStarted = false;

public:
    /** 供 ControlPanel 调用:显示/隐藏"显示控制台"小按钮 */
    void showToggleCtrlBtn(bool show);
    QToolButton *toggleCtrlButton() const { return m_toggleCtrlBtn; }

signals:
    void sessionCountChanged(int n);
    void statusMessage(const QString &msg);
    /** 用户点击了"显示控制台"小按钮(由 ControlPanel 监听) */
    void requestShowControlPanel();
};
