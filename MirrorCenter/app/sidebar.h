#pragma once

#include <QWidget>
#include <QButtonGroup>
#include <QString>
#include <QList>

class QVBoxLayout;
class QScrollArea;
class QToolButton;
class QLabel;

/**
 * 左侧导航栏(EdgeCast Studio 风格)
 * - 顶部 Logo + 应用名 + 副标题
 * - 中部可滚动分组导航
 * - 每组:组标题(可折叠) + 若干导航项(单选)
 * - 选中态蓝紫渐变高亮
 *
 * 信号:
 *   navItemClicked(QString key)  某个导航项被点击
 */
class Sidebar : public QWidget
{
    Q_OBJECT
public:
    struct NavItem {
        QString key;       // 唯一标识,用于路由到对应页面
        QString icon;      // 图标(目前用 Unicode 字符)
        QString text;      // 显示文本
    };

    struct NavGroup {
        QString title;                // 组标题
        bool    collapsible = true;   // 是否可折叠
        bool    expanded    = true;   // 初始是否展开
        QList<NavItem> items;         // 导航项
    };

    explicit Sidebar(QWidget *parent = nullptr);

    /** 设置导航结构(整组替换) */
    void setGroups(const QList<NavGroup> &groups);

    /** 选中指定 key 的导航项(不触发信号) */
    void selectKey(const QString &key);

    /** 当前选中 key */
    QString currentKey() const;

    QSize sizeHint() const override;

signals:
    void navItemClicked(const QString &key);

private slots:
    void onGroupHeaderClicked();
    void onNavItemClicked(QAbstractButton *btn);

private:
    void buildHeader();
    void rebuildNavList();
    QToolButton *createNavButton(const NavItem &item);

    QWidget   *m_header        = nullptr;
    QLabel    *m_logoLabel     = nullptr;
    QLabel    *m_titleLabel    = nullptr;
    QLabel    *m_taglineLabel  = nullptr;
    QScrollArea *m_scroll      = nullptr;
    QWidget   *m_scrollContent = nullptr;
    QVBoxLayout *m_scrollLayout = nullptr;

    QList<NavGroup> m_groups;
    QButtonGroup *m_btnGroup = nullptr;
    QString m_currentKey;
};
