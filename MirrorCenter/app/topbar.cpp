#include "topbar.h"

#include <QHBoxLayout>
#include <QComboBox>
#include <QToolButton>
#include <QLabel>
#include <QSizePolicy>

TopBar::TopBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("topBarRoot");
    setFixedHeight(56);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(20, 8, 20, 8);
    root->setSpacing(0);

    // 左侧 spacer(让中间靠左对齐而非严格居中,跟参考一致)
    auto *leftSpacer = new QWidget(this);
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    leftSpacer->setMinimumWidth(0);
    root->addWidget(leftSpacer);

    // -------- 居中:布局选择器(胶囊) --------
    auto *centerWrap = new QWidget(this);
    centerWrap->setObjectName("topBarCenter");
    auto *cl = new QHBoxLayout(centerWrap);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(8);

    auto *prefix = new QLabel(QStringLiteral("当前布局:"), centerWrap);
    prefix->setStyleSheet(
        "color:#8A93A6; font-size:13px; background-color:transparent;");
    cl->addWidget(prefix);

    m_layoutCombo = new QComboBox(centerWrap);
    m_layoutCombo->setObjectName("projectSelector");
    m_layoutCombo->addItems({
        QStringLiteral("单屏模式"),
        QStringLiteral("双拼模式"),
        QStringLiteral("三分屏模式"),
        QStringLiteral("四宫格模式"),
        QStringLiteral("六宫格模式"),
        QStringLiteral("自定义模式"),
    });
    m_layoutCombo->setMinimumWidth(150);
    cl->addWidget(m_layoutCombo);

    root->addWidget(centerWrap);

    // 中右 spacer
    auto *midSpacer = new QWidget(this);
    midSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    midSpacer->setMinimumWidth(40);
    root->addWidget(midSpacer);

    // -------- 右侧:5 个操作按钮 --------
    auto *rightWrap = new QWidget(this);
    rightWrap->setObjectName("topBarRight");
    auto *rl = new QHBoxLayout(rightWrap);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(6);

    struct ActionDef { QToolButton **btn; QString icon; QString text; };
    const ActionDef defs[] = {
        { &m_btnFullscreen,  QStringLiteral("⛶"), QStringLiteral("全屏")   },
        { &m_btnRecordScreen,QStringLiteral("⏺"), QStringLiteral("录屏")   },
        { &m_btnRecord,      QStringLiteral("●"), QStringLiteral("录制")   },
        { &m_btnHelp,        QStringLiteral("?"), QStringLiteral("投屏帮助") },
        { &m_btnMore,        QStringLiteral("⋯"), QStringLiteral("更多")   },
    };
    for (const ActionDef &d : defs) {
        auto *btn = new QToolButton(rightWrap);
        btn->setObjectName("topActionButton");
        // 图标 + 文字两行排列(模仿参考图)
        btn->setText(QStringLiteral("%1\n%2").arg(d.icon, d.text));
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setToolTip(d.text);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setMinimumWidth(56);
        btn->setMinimumHeight(40);
        *(d.btn) = btn;
        rl->addWidget(btn);
    }

    root->addWidget(rightWrap);

    // 信号
    connect(m_layoutCombo, &QComboBox::currentTextChanged,
            this, &TopBar::layoutOptionChanged);
    connect(m_btnFullscreen,   &QToolButton::clicked, this, &TopBar::fullscreenClicked);
    connect(m_btnRecordScreen, &QToolButton::clicked, this, &TopBar::recordScreenClicked);
    connect(m_btnRecord,       &QToolButton::clicked, this, &TopBar::recordClicked);
    connect(m_btnHelp,         &QToolButton::clicked, this, &TopBar::helpClicked);
    connect(m_btnMore,         &QToolButton::clicked, this, &TopBar::moreClicked);
}

void TopBar::setLayoutOptions(const QStringList &options, int currentIndex)
{
    if (options.isEmpty())
        return;
    m_layoutCombo->clear();
    m_layoutCombo->addItems(options);
    if (currentIndex >= 0 && currentIndex < options.size())
        m_layoutCombo->setCurrentIndex(currentIndex);
}

void TopBar::setCurrentLayoutText(const QString &text)
{
    int idx = m_layoutCombo->findText(text);
    if (idx >= 0)
        m_layoutCombo->setCurrentIndex(idx);
}
