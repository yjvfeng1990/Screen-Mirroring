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

    // -------- 右侧:全屏按钮 --------
    auto *rightWrap = new QWidget(this);
    rightWrap->setObjectName("topBarRight");
    auto *rl = new QHBoxLayout(rightWrap);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(6);

    m_btnFullscreen = new QToolButton(rightWrap);
    m_btnFullscreen->setObjectName("topActionButton");
    m_btnFullscreen->setText(QStringLiteral("⛶\n全屏"));
    m_btnFullscreen->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_btnFullscreen->setToolTip(QStringLiteral("全屏"));
    m_btnFullscreen->setCursor(Qt::PointingHandCursor);
    m_btnFullscreen->setMinimumWidth(56);
    m_btnFullscreen->setMinimumHeight(40);
    rl->addWidget(m_btnFullscreen);

    root->addWidget(rightWrap);

    // 信号
    connect(m_layoutCombo, &QComboBox::currentTextChanged,
            this, &TopBar::layoutOptionChanged);
    connect(m_btnFullscreen, &QToolButton::clicked, this, &TopBar::fullscreenClicked);
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
