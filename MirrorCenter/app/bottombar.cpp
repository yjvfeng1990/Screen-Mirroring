#include "bottombar.h"
#include "systemstatus.h"
#include "volumecontrol.h"

#include <QHBoxLayout>
#include <QToolButton>
#include <QButtonGroup>
#include <QFrame>
#include <QDebug>

BottomControlBar::BottomControlBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("bottomBarRoot");
    setFixedHeight(92);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(16, 10, 16, 10);
    root->setSpacing(16);

    // 左侧折叠按钮
    m_collapseBtn = new QToolButton(this);
    m_collapseBtn->setObjectName("bottomCollapseBtn");
    m_collapseBtn->setText(QStringLiteral("◀"));
    m_collapseBtn->setToolTip(QStringLiteral("折叠/展开"));
    m_collapseBtn->setCursor(Qt::PointingHandCursor);
    m_collapseBtn->setFixedSize(28, 28);
    connect(m_collapseBtn, &QToolButton::clicked, this, [this]() {
        static bool collapsed = false;
        collapsed = !collapsed;
        m_collapseBtn->setText(collapsed ? QStringLiteral("▶") : QStringLiteral("◀"));
        emit collapseToggled(collapsed);
    });
    root->addWidget(m_collapseBtn, 0, Qt::AlignBottom);

    // 左侧:系统状态
    m_status = new SystemStatus(this);
    root->addWidget(m_status, 0, Qt::AlignBottom);

    // 中央:布局选择胶囊
    buildLayoutPill();
    root->addWidget(m_pill, 1, Qt::AlignCenter);

    // 右侧:音量控制
    m_volume = new VolumeControl(this);
    root->addWidget(m_volume, 0, Qt::AlignBottom);
}

void BottomControlBar::buildLayoutPill()
{
    m_pill = new QFrame(this);
    m_pill->setObjectName("layoutPill");
    auto *pl = new QHBoxLayout(m_pill);
    pl->setContentsMargins(6, 4, 6, 4);
    pl->setSpacing(4);

    m_btnGroup = new QButtonGroup(this);
    m_btnGroup->setExclusive(true);

    struct BtnDef { int mode; QString icon; QString text; };
    const BtnDef defs[] = {
        { 1,  QStringLiteral("▢"), QStringLiteral("单屏")   },
        { 2,  QStringLiteral("◫"), QStringLiteral("双拼")   },
        { 3,  QStringLiteral("▦"), QStringLiteral("三分屏") },
        { 4,  QStringLiteral("▤"), QStringLiteral("四宫格") },
        { 6,  QStringLiteral("▩"), QStringLiteral("六宫格") },
        { 99, QStringLiteral("✎"), QStringLiteral("自定义") },
    };
    for (const BtnDef &d : defs) {
        auto *btn = new QToolButton(m_pill);
        btn->setObjectName("layoutPillButton");
        btn->setText(QStringLiteral("  %1\n%2  ").arg(d.icon, d.text));
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setMinimumWidth(72);
        btn->setMinimumHeight(48);
        btn->setToolTip(d.text);
        btn->setProperty("layoutMode", d.mode);
        pl->addWidget(btn);
        m_btnGroup->addButton(btn, d.mode);
        if (d.mode == m_currentMode)
            btn->setChecked(true);
    }

    connect(m_btnGroup, &QButtonGroup::idClicked, this,
            [this](int id) {
                m_currentMode = id;
                emit layoutModeChanged(id);
            });
}

void BottomControlBar::setLayoutMode(int mode)
{
    m_currentMode = mode;
    if (auto *btn = m_btnGroup->button(mode))
        btn->setChecked(true);
}
