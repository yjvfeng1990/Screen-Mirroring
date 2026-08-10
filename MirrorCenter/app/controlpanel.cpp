#include "controlpanel.h"
#include "sidebar.h"
#include "topbar.h"
#include "bottombar.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QToolButton>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QDebug>

ControlPanel::ControlPanel(QWidget *parent)
    : QWidget(parent)
{
    // 无边框 + 置顶 + 工具窗口(不出现在任务栏)
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);  // 外层透明,圆角由 m_rootFrame 实现
    setMinimumSize(820, 540);
    resize(900, 600);

    buildUi();
    wireSignals();

    // 阴影
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 160));
    m_rootFrame->setGraphicsEffect(shadow);
}

ControlPanel::~ControlPanel() = default;

void ControlPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);  // 给阴影留空间
    outer->setSpacing(0);

    // 外层圆角深色面板
    m_rootFrame = new QFrame(this);
    m_rootFrame->setObjectName("ctrlPanelRoot");
    auto *rfL = new QVBoxLayout(m_rootFrame);
    rfL->setContentsMargins(0, 0, 0, 0);
    rfL->setSpacing(0);

    // 标题栏
    buildTitleBar();
    rfL->addWidget(m_titleBar);

    // 内容区(Sidebar 左,TopBar+BottomBar 右)
    auto *content = new QWidget(m_rootFrame);
    content->setObjectName("ctrlPanelContent");
    auto *contentL = new QHBoxLayout(content);
    contentL->setContentsMargins(0, 0, 0, 0);
    contentL->setSpacing(0);

    m_sidebar = new Sidebar(content);
    contentL->addWidget(m_sidebar);

    auto *right = new QWidget(content);
    auto *rL = new QVBoxLayout(right);
    rL->setContentsMargins(0, 0, 0, 0);
    rL->setSpacing(0);

    m_topBar = new TopBar(right);
    rL->addWidget(m_topBar);

    m_bottomBar = new BottomControlBar(right);
    rL->addWidget(m_bottomBar);

    contentL->addWidget(right, 1);
    rfL->addWidget(content, 1);

    outer->addWidget(m_rootFrame);
}

void ControlPanel::buildTitleBar()
{
    m_titleBar = new QFrame();
    m_titleBar->setObjectName("ctrlPanelTitleBar");
    m_titleBar->setFixedHeight(32);
    auto *tl = new QHBoxLayout(m_titleBar);
    tl->setContentsMargins(14, 0, 6, 0);
    tl->setSpacing(6);

    // 小图标
    auto *ico = new QLabel(QStringLiteral("🎛"));
    ico->setStyleSheet("background-color:transparent; font-size:14px;");
    tl->addWidget(ico);

    m_titleLabel = new QLabel(QStringLiteral("EdgeCast Studio · 控制台"));
    m_titleLabel->setStyleSheet(
        "color:#C2C9D6; font-size:12px; font-weight:600;"
        "background-color:transparent;");
    tl->addWidget(m_titleLabel);
    tl->addStretch(1);

    m_btnPin = new QToolButton();
    m_btnPin->setObjectName("ctrlPanelTitleBtn");
    m_btnPin->setText(QStringLiteral("📌"));
    m_btnPin->setToolTip(QStringLiteral("置顶/取消置顶"));
    m_btnPin->setFixedSize(24, 24);
    m_btnPin->setCursor(Qt::PointingHandCursor);
    tl->addWidget(m_btnPin);

    m_btnClose = new QToolButton();
    m_btnClose->setObjectName("ctrlPanelTitleBtn");
    m_btnClose->setText(QStringLiteral("×"));
    m_btnClose->setToolTip(QStringLiteral("隐藏控制台(右下角按钮可重新唤出)"));
    m_btnClose->setFixedSize(24, 24);
    m_btnClose->setCursor(Qt::PointingHandCursor);
    tl->addWidget(m_btnClose);
}

void ControlPanel::wireSignals()
{
    // Sidebar → 转发
    connect(m_sidebar, &Sidebar::navItemClicked,
            this, &ControlPanel::navItemClicked);

    // TopBar → 转发
    connect(m_topBar, &TopBar::layoutOptionChanged, this, [this](const QString &text) {
        int mode = 1;
        if      (text.contains(QStringLiteral("单屏")))   mode = 1;
        else if (text.contains(QStringLiteral("双拼")))   mode = 2;
        else if (text.contains(QStringLiteral("三分屏"))) mode = 3;
        else if (text.contains(QStringLiteral("四宫格"))) mode = 4;
        else if (text.contains(QStringLiteral("六宫格"))) mode = 6;
        else if (text.contains(QStringLiteral("自定义"))) mode = 99;
        m_currentMode = mode;
        m_bottomBar->setLayoutMode(mode);
        emit layoutModeChanged(mode);
    });
    connect(m_topBar, &TopBar::fullscreenClicked,
            this, &ControlPanel::fullscreenToggleRequested);
    connect(m_topBar, &TopBar::recordScreenClicked,
            this, &ControlPanel::recordScreenRequested);
    connect(m_topBar, &TopBar::recordClicked,
            this, &ControlPanel::recordRequested);
    connect(m_topBar, &TopBar::helpClicked,
            this, &ControlPanel::helpRequested);
    connect(m_topBar, &TopBar::moreClicked,
            this, &ControlPanel::moreRequested);

    // BottomBar → 转发
    connect(m_bottomBar, &BottomControlBar::layoutModeChanged,
            this, [this](int mode) {
                m_currentMode = mode;
                m_topBar->setCurrentLayoutText(
                    [mode]() {
                        switch (mode) {
                            case 1:  return QStringLiteral("单屏模式");
                            case 2:  return QStringLiteral("双拼模式");
                            case 3:  return QStringLiteral("三分屏模式");
                            case 4:  return QStringLiteral("四宫格模式");
                            case 6:  return QStringLiteral("六宫格模式");
                            default: return QStringLiteral("自定义模式");
                        }
                    }());
                emit layoutModeChanged(mode);
            });
    connect(m_bottomBar, &BottomControlBar::collapseToggled,
            this, &ControlPanel::bottomCollapseToggled);

    // 标题栏按钮
    connect(m_btnClose, &QToolButton::clicked, this, [this]() {
        setPanelVisible(false);
        emit hideRequested();
    });
    connect(m_btnPin, &QToolButton::clicked, this, [this]() {
        bool onTop = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
        Qt::WindowFlags f = windowFlags();
        if (onTop) f &= ~Qt::WindowStaysOnTopHint;
        else       f |=  Qt::WindowStaysOnTopHint;
        // Tool 标志需要保持
        f |= Qt::Tool;
        setWindowFlags(f);
        show();  // 改 flags 后需要重新 show
        m_btnPin->setText(onTop ? QStringLiteral("📍") : QStringLiteral("📌"));
    });
}

void ControlPanel::setNavGroups(const QList<Sidebar::NavGroup> &groups)
{
    m_sidebar->setGroups(groups);
}

void ControlPanel::selectNavKey(const QString &key)
{
    m_sidebar->selectKey(key);
}

void ControlPanel::setLayoutMode(int mode)
{
    m_currentMode = mode;
    m_bottomBar->setLayoutMode(mode);
    QString text;
    switch (mode) {
        case 1:  text = QStringLiteral("单屏模式"); break;
        case 2:  text = QStringLiteral("双拼模式"); break;
        case 3:  text = QStringLiteral("三分屏模式"); break;
        case 4:  text = QStringLiteral("四宫格模式"); break;
        case 6:  text = QStringLiteral("六宫格模式"); break;
        default: text = QStringLiteral("自定义模式"); break;
    }
    m_topBar->setCurrentLayoutText(text);
}

int ControlPanel::layoutMode() const
{
    return m_currentMode;
}

void ControlPanel::setPanelVisible(bool visible)
{
    if (visible) {
        show();
        raise();
    } else {
        hide();
    }
}

void ControlPanel::mousePressEvent(QMouseEvent *e)
{
    // 拖动:仅响应标题栏区域的左键
    if (e->button() == Qt::LeftButton) {
        QPoint local = m_titleBar->mapFrom(this, e->pos());
        if (m_titleBar->rect().contains(local)) {
            m_dragging = true;
            m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
            e->accept();
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void ControlPanel::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPosition().toPoint() - m_dragOffset);
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void ControlPanel::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void ControlPanel::mouseDoubleClickEvent(QMouseEvent *e)
{
    // 双击标题栏 = 切换全屏
    QPoint local = m_titleBar->mapFrom(this, e->pos());
    if (m_titleBar->rect().contains(local)) {
        emit fullscreenToggleRequested();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}
