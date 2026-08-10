#include "sidebar.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QButtonGroup>
#include <QScrollArea>
#include <QFrame>
#include <QSpacerItem>
#include <QDebug>

namespace {
constexpr int kItemHeight = 36;
}

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("sidebarRoot");
    setFixedWidth(220);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // -------- 顶部 Logo 区 --------
    buildHeader();
    root->addWidget(m_header);

    // -------- 中部滚动导航 --------
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("sidebarScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setProperty("class", "sidebarScrollArea");

    m_scrollContent = new QWidget();
    m_scrollContent->setObjectName("sidebarScrollContent");
    m_scrollLayout = new QVBoxLayout(m_scrollContent);
    m_scrollLayout->setContentsMargins(0, 8, 0, 12);
    m_scrollLayout->setSpacing(0);
    m_scrollLayout->addStretch(1);

    m_scroll->setWidget(m_scrollContent);
    root->addWidget(m_scroll, 1);

    m_btnGroup = new QButtonGroup(this);
    m_btnGroup->setExclusive(true);
    connect(m_btnGroup,
            QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
            this,
            &Sidebar::onNavItemClicked);
}

void Sidebar::buildHeader()
{
    m_header = new QWidget(this);
    m_header->setObjectName("sidebarHeader");
    auto *hl = new QVBoxLayout(m_header);
    hl->setContentsMargins(16, 20, 16, 18);
    hl->setSpacing(4);

    m_logoLabel = new QLabel(m_header);
    m_logoLabel->setObjectName("sidebarLogo");
    m_logoLabel->setText(QStringLiteral("📺  EdgeCast Studio"));
    hl->addWidget(m_logoLabel);

    m_taglineLabel = new QLabel(m_header);
    m_taglineLabel->setObjectName("sidebarTagline");
    m_taglineLabel->setText(QStringLiteral("多端投屏演示控制台"));
    hl->addWidget(m_taglineLabel);
}

QSize Sidebar::sizeHint() const
{
    return QSize(220, 600);
}

QToolButton *Sidebar::createNavButton(const NavItem &item)
{
    auto *btn = new QToolButton(m_scrollContent);
    btn->setObjectName("sidebarNavItem");
    btn->setText(QStringLiteral("  %1   %2").arg(item.icon, item.text));
    btn->setCheckable(true);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setMinimumHeight(kItemHeight);
    btn->setToolTip(item.text);
    return btn;
}

void Sidebar::setGroups(const QList<NavGroup> &groups)
{
    m_groups = groups;
    rebuildNavList();
}

void Sidebar::rebuildNavList()
{
    // 1) 清空旧控件(保留最后的 stretch)
    QLayoutItem *child = nullptr;
    while ((child = m_scrollLayout->takeAt(0)) != nullptr) {
        if (auto *w = child->widget()) {
            if (auto *btn = qobject_cast<QAbstractButton *>(w)) {
                m_btnGroup->removeButton(btn);
            }
            w->deleteLater();
        }
        delete child;
    }

    // 2) 重新构建
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        const NavGroup &g = m_groups[gi];

        // 组标题
        if (g.collapsible) {
            auto *header = new QToolButton(m_scrollContent);
            header->setObjectName("sidebarGroupHeader");
            const QString arrow = g.expanded ? QStringLiteral("▼") : QStringLiteral("▶");
            header->setText(QStringLiteral("  %1   %2").arg(arrow, g.title));
            header->setCheckable(true);
            header->setChecked(g.expanded);
            header->setToolButtonStyle(Qt::ToolButtonTextOnly);
            header->setCursor(Qt::PointingHandCursor);
            header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            header->setProperty("groupIndex", gi);
            m_scrollLayout->addWidget(header);
            connect(header, &QToolButton::toggled, this,
                    [this, header, gi](bool checked) {
                        m_groups[gi].expanded = checked;
                        header->setText(QStringLiteral("  %1   %2")
                                            .arg(checked ? QStringLiteral("▼")
                                                          : QStringLiteral("▶"),
                                                 m_groups[gi].title));
                        // 折叠/展开:简单做法是重建列表
                        rebuildNavList();
                    });
        } else {
            auto *title = new QLabel(m_scrollContent);
            title->setObjectName("sidebarGroupHeader");
            title->setText(g.title);
            title->setProperty("plainHeader", true);
            m_scrollLayout->addWidget(title);
        }

        // 导航项
        if (!g.collapsible || g.expanded) {
            for (const NavItem &it : g.items) {
                QToolButton *btn = createNavButton(it);
                m_scrollLayout->addWidget(btn);
                m_btnGroup->addButton(btn);
                btn->setProperty("navKey", it.key);
                if (it.key == m_currentKey) {
                    btn->setChecked(true);
                }
            }
        }
    }

    m_scrollLayout->addStretch(1);
}

void Sidebar::selectKey(const QString &key)
{
    if (key == m_currentKey)
        return;
    m_currentKey = key;
    // 仅更新选中态,不重建列表
    for (QAbstractButton *btn : m_btnGroup->buttons()) {
        if (btn->property("navKey").toString() == key) {
            btn->setChecked(true);
            break;
        }
    }
}

QString Sidebar::currentKey() const
{
    return m_currentKey;
}

void Sidebar::onNavItemClicked(QAbstractButton *btn)
{
    const QString key = btn->property("navKey").toString();
    if (key.isEmpty())
        return;
    m_currentKey = key;
    emit navItemClicked(key);
}

void Sidebar::onGroupHeaderClicked()
{
    // 已用 lambda 直接处理
}
