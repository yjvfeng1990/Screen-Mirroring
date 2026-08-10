#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>
#include <QList>

#include "sidebar.h"

class TopBar;
class BottomControlBar;
class QVBoxLayout;
class QHBoxLayout;
class QToolButton;
class QFrame;

/**
 * 悬浮控制台(无边框 + 置顶 + 可拖动)
 * - 内部三区:Sidebar(左) / TopBar(上) / BottomControlBar(下)
 * - 整体由一张半透明深色圆角面板承载,玻璃拟态感
 * - 标题栏可拖动整个面板
 * - 右上角"×"按钮 = 隐藏(DesktopWindow 上会浮现"显示控制台"小按钮)
 *
 * 信号:把所有操作转发给 DesktopWindow
 */
class ControlPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ControlPanel(QWidget *parent = nullptr);
    ~ControlPanel() override;

    /** 构建导航分组(转给 Sidebar) */
    void setNavGroups(const QList<Sidebar::NavGroup> &groups);
    void selectNavKey(const QString &key);

    /** 同步当前布局模式到内部 TopBar/BottomBar */
    void setLayoutMode(int mode);
    int  layoutMode() const;

    /** 显示/隐藏动画(简单) */
    void setPanelVisible(bool show);
    bool isPanelVisible() const { return !isHidden(); }

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
    void buildUi();
    void buildTitleBar();
    void wireSignals();
    void applyLayout();

    QFrame         *m_rootFrame     = nullptr;  // 外层圆角面板
    QFrame         *m_titleBar      = nullptr;  // 标题栏(可拖动)
    QToolButton    *m_btnClose      = nullptr;  // 隐藏按钮
    QToolButton    *m_btnPin        = nullptr;  // 置顶切换
    QLabel         *m_titleLabel    = nullptr;
    Sidebar        *m_sidebar       = nullptr;
    TopBar         *m_topBar        = nullptr;
    BottomControlBar *m_bottomBar   = nullptr;
    int             m_currentMode   = 1;
    bool            m_dragging      = false;
    QPoint          m_dragOffset;

signals:
    /** 导航项被点击 */
    void navItemClicked(const QString &key);
    /** 布局模式改变 1/2/3/4/6/99 */
    void layoutModeChanged(int mode);
    /** 用户请求关闭(隐藏)控制台 */
    void hideRequested();
    /** 全屏切换请求 */
    void fullscreenToggleRequested();
    /** 投屏帮助 */
    void helpRequested();
    /** 录屏 / 录制 / 更多 等操作 */
    void recordScreenRequested();
    void recordRequested();
    void moreRequested();
    /** 折叠底部 */
    void bottomCollapseToggled(bool collapsed);
};
