#include "desktopwindow.h"
#include "sessionview.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QStatusBar>
#include <QToolButton>
#include <QShortcut>
#include <QKeySequence>
#include <QNetworkInterface>
#include <QAbstractSocket>
#include <QTimer>
#include <QDebug>
#include <QMouseEvent>

DesktopWindow::DesktopWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("MirrorCenter 投屏接收中心"));
    resize(1280, 800);
    setMinimumSize(800, 500);
    // 黑底:投屏画面的最佳承载背景
    setStyleSheet("background-color: #05080F;");

    buildUi();

    // 快捷键:F11 = 全屏切换
    auto *fs = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fs, &QShortcut::activated, this, &DesktopWindow::toggleFullscreen);
    // ESC 退出全屏
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (isFullScreen()) showNormal();
    });
}

DesktopWindow::~DesktopWindow()
{
    for (SessionView *view : m_views)
        view->stop();
}

void DesktopWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 中央画布 ----
    m_canvasInner = new QWidget(this);
    m_canvasInner->setStyleSheet("background-color: #05080F;");
    auto *innerL = new QVBoxLayout(m_canvasInner);
    innerL->setContentsMargins(12, 12, 12, 12);
    innerL->setSpacing(0);
    m_grid = new QGridLayout();
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(8);
    innerL->addLayout(m_grid);
    root->addWidget(m_canvasInner, 1);

    // ---- 空状态卡片 ----
    QStringList ipList;
    const auto allAddrs = QNetworkInterface::allAddresses();
    for (const QHostAddress &a : allAddrs) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            ipList << a.toString();
    }
    const QString ipText = ipList.isEmpty()
        ? QStringLiteral("(自动获取中)")
        : ipList.join(QStringLiteral("  /  "));

    m_emptyCard = new QFrame();
    m_emptyCard->setObjectName("emptyCard");
    auto *el = new QVBoxLayout(m_emptyCard);
    el->setContentsMargins(60, 40, 60, 40);
    el->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("📺  MirrorCenter 投屏接收中心"));
    title->setStyleSheet("color:#FFFFFF; font-size:22px; font-weight:700;"
                         "background-color:transparent;");
    title->setAlignment(Qt::AlignCenter);
    el->addWidget(title);

    auto *sub = new QLabel(QStringLiteral("服务已开启 · 等待设备投屏连接"));
    sub->setStyleSheet("color:#8A93A6; font-size:13px; background-color:transparent;");
    sub->setAlignment(Qt::AlignCenter);
    el->addWidget(sub);

    el->addSpacing(8);

    m_emptyLabel = new QLabel(QStringLiteral(
        "① iPhone / iPad(AirPlay)\n"
        "   · 控制中心 → 屏幕镜像 → 选择 MirrorCenter\n\n"
        "② 安卓 / Windows(Miracast)\n"
        "   · 安卓:设置 → 无线投屏\n"
        "   · Windows:Win + K → 连接无线显示器\n\n"
        "本机 IP:%1\n"
        "支持多路同时投屏 · 右下角控制台切换布局")
        .arg(ipText));
    m_emptyLabel->setObjectName("emptyLabel");
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    el->addWidget(m_emptyLabel);

    m_grid->addWidget(m_emptyCard, 0, 0);

    // ---- 右下角"显示控制台"小按钮(控制台隐藏时显示) ----
    m_toggleCtrlBtn = new QToolButton(this);
    m_toggleCtrlBtn->setObjectName("toggleCtrlBtn");
    m_toggleCtrlBtn->setText(QStringLiteral("☰  显示控制台"));
    m_toggleCtrlBtn->setCursor(Qt::PointingHandCursor);
    m_toggleCtrlBtn->setFixedSize(120, 32);
    m_toggleCtrlBtn->hide();
    m_toggleCtrlBtn->raise();
}

void DesktopWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_toggleCtrlBtn && !m_toggleCtrlBtn->isHidden()) {
        m_toggleCtrlBtn->move(width() - m_toggleCtrlBtn->width() - 16,
                              height() - m_toggleCtrlBtn->height() - 16);
    }
}

void DesktopWindow::setLayoutMode(int mode)
{
    if (mode == 99) {
        emit statusMessage(QStringLiteral("自定义布局:请直接拖拽会话卡片调整"));
        return;
    }
    m_layoutMode = mode;
    relayout();
}

void DesktopWindow::relayout()
{
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(nullptr);
            w->hide();
        }
        delete item;
    }

    if (m_views.isEmpty()) {
        if (m_emptyCard) {
            m_emptyCard->setParent(m_canvasInner);
            m_emptyCard->show();
            m_grid->addWidget(m_emptyCard, 0, 0);
        }
        emit statusMessage(QStringLiteral("无会话"));
        emit sessionCountChanged(0);
        return;
    }

    if (m_emptyCard) {
        m_emptyCard->hide();
        m_emptyCard->setParent(nullptr);
    }

    int cols = 1;
    switch (m_layoutMode) {
        case 1: cols = 1; break;
        case 2: cols = 2; break;
        case 3: cols = 3; break;
        case 4: cols = 2; break;
        case 6: cols = 3; break;
        default: cols = qMin(m_views.size(), 3); break;
    }
    cols = qMin(cols, m_views.size());

    int row = 0, col = 0;
    for (SessionView *view : m_views) {
        m_grid->addWidget(view, row, col);
        view->show();
        if (++col >= cols) { col = 0; ++row; }
    }
    emit statusMessage(QStringLiteral("会话数: %1  ·  布局: %2 路")
                           .arg(m_views.size()).arg(m_layoutMode));
    emit sessionCountChanged(m_views.size());
}

void DesktopWindow::startAirPlay()
{
    if (m_airplayStarted) return;
    addSession(QStringLiteral("MirrorCenter"), MIRROR_BACKEND_AIRPLAY);
    m_airplayStarted = true;
    emit statusMessage(QStringLiteral("AirPlay 接收已启动"));
}

void DesktopWindow::startMiracast()
{
    if (m_miracastStarted) return;
    addSession(QStringLiteral("MirrorCenter-Miracast"), MIRROR_BACKEND_MIRACAST);
    m_miracastStarted = true;
    emit statusMessage(QStringLiteral("Miracast 接收已启动"));
}

SessionView *DesktopWindow::addSession(const QString &name, mirror_backend_t backend)
{
    auto *view = new SessionView(name, backend, m_canvasInner);
    connect(view, &SessionView::sessionClosed,
            this, &DesktopWindow::onSessionClosed);
    m_views.append(view);
    relayout();
    return view;
}

void DesktopWindow::onSessionClosed(const QString &sessionId)
{
    for (int i = 0; i < m_views.size(); ++i) {
        if (m_views[i]->sessionId() == sessionId) {
            const bool wasAir   = (m_views[i]->deviceName() == QStringLiteral("MirrorCenter"));
            const bool wasMira  = (m_views[i]->deviceName() == QStringLiteral("MirrorCenter-Miracast"));
            m_views[i]->deleteLater();
            m_views.removeAt(i);
            if (wasAir)  m_airplayStarted  = false;
            if (wasMira) m_miracastStarted = false;
            break;
        }
    }
    relayout();
}

void DesktopWindow::toggleFullscreen()
{
    if (isFullScreen()) showNormal();
    else                showFullScreen();
}

/** 显示控制台小按钮的可见性控制 */
void DesktopWindow::showToggleCtrlBtn(bool show)
{
    if (!m_toggleCtrlBtn) return;
    if (show) {
        m_toggleCtrlBtn->show();
        m_toggleCtrlBtn->raise();
        m_toggleCtrlBtn->move(width() - m_toggleCtrlBtn->width() - 16,
                              height() - m_toggleCtrlBtn->height() - 16);
    } else {
        m_toggleCtrlBtn->hide();
    }
}
