#pragma once

#include <QWidget>
#include <QString>
#include <QList>
#include <QHash>
#include <QPointer>
#include <functional>

class QVBoxLayout;
class QToolButton;
class QFrame;
class QLabel;
class QTimer;
class QPixmap;

/**
 * 侧边吸附"投屏来源/播放器窗口"面板(参考 TopDesk 侧边栏交互)
 * - 停靠在屏幕右边缘, 平时收起只露出 8px 边缘触发条
 * - 鼠标悬停触发条或面板 → 展开; 鼠标离开面板 300ms → 自动收起
 * - 内容: 播放器窗口列表(缩略图 + 名称/IP/状态), 点击选中 → 该窗口在主窗口首位
 * - 标题栏"×" = 收起到边缘(触发条可再次展开)
 */
class ControlPanel : public QWidget
{
    Q_OBJECT
public:
    /** 单个来源条目(播放器窗口) */
    struct SourceInfo {
        QString sessionId;  // 会话标识(选中置顶/缩略图定位用)
        QString name;       // 来源名称(手机名或 IP)
        QString ip;         // 客户端 IP
        QString status;     // 状态文本
    };

    explicit ControlPanel(QWidget *parent = nullptr);
    ~ControlPanel() override;

    /** 刷新来源列表(重建卡片;已有卡片保留以便平滑更新缩略图) */
    void setSources(const QList<SourceInfo> &sources);

    /** 设置缩略图获取函数(由 main 提供, 查 DesktopWindow) */
    void setThumbnailProvider(std::function<QPixmap(const QString &)> provider);

    /** 展开/收起吸附面板 */
    void setPanelVisible(bool show);
    bool isPanelVisible() const;

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void buildUi();
    void buildTitleBar();
    void expand();
    void collapse();
    void updateDockGeometry();
    void updateTriggerPosition();
    /** 刷新所有卡片缩略图(节拍触发) */
    void refreshThumbnails();
    /** 列表项被点击(选中置顶) */
    void onItemClicked(const QString &sessionId);
    /** 刷新选中高亮 */
    void updateSelection();

    QFrame       *m_rootFrame    = nullptr;  // 外层圆角面板
    QFrame       *m_titleBar     = nullptr;  // 标题栏
    QToolButton  *m_btnClose     = nullptr;  // 收起到边缘
    QLabel       *m_titleLabel   = nullptr;
    QWidget      *m_listHost     = nullptr;  // 来源列表容器
    QVBoxLayout  *m_listLayout   = nullptr;
    QLabel       *m_emptyLabel   = nullptr;  // 空状态提示

    // 列表卡片:sessionId → 缩略图标签
    QHash<QString, QPointer<QLabel>> m_thumbLabels;
    QString m_selectedId;                      // 当前选中(置顶)的会话
    std::function<QPixmap(const QString &)> m_thumbProvider;
    QTimer *m_thumbTimer = nullptr;            // 缩略图刷新节拍(800ms)

    // 侧边吸附
    QWidget *m_triggerButton = nullptr;   // 收起时屏幕右边缘的触发条(独立置顶小窗)
    QTimer  *m_hideTimer     = nullptr;   // 鼠标离开 300ms 后收起
    bool     m_dockedExpanded = true;     // 当前是否展开
    int      m_panelWidth     = 320;
    int      m_collapsedWidth = 8;

signals:
    /** 用户请求收起控制台 */
    void hideRequested();
    /** 用户点击列表项:选中该来源窗口, 由 DesktopWindow 置顶 */
    void sourceSelected(const QString &sessionId);
};
