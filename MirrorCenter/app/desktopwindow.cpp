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
#include <QMetaObject>

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
    m_canvasLayout = new QVBoxLayout(m_canvasInner);
    m_canvasLayout->setContentsMargins(12, 12, 12, 12);
    m_canvasLayout->setSpacing(0);
    m_grid = new QGridLayout();
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(8);
    m_canvasLayout->addLayout(m_grid);
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
    const int maxG = maxGrid();

    // 只统计已激活(有真实画面)的会话:AirPlay 连接即激活;
    // Miracast 占位会话在收到首帧前保持隐藏, 不出现在主窗口/列表。
    QList<SessionView *> active;
    for (SessionView *view : m_views) {
        if (view->isActive())
            active.append(view);
    }

    const int n = qMin(active.size(), maxG);

    // 布局列数:0=按会话数自动(1全屏/2左右/3~4四宫格), 手动模式尊重控制台选择
    // 受解码能力限制:软解最多 4 格, 硬解最多 16 格
    int cols = 2, rows = (n + 1) / 2;
    switch (m_layoutMode) {
        case 0:
            if (n == 1)      { cols = 1; rows = 1; }
            else if (n == 2) { cols = 2; rows = 1; }
            else if (n <= 4) { cols = 2; rows = 2; }
            else if (n <= 6) { cols = 3; rows = 2; }
            else if (n <= 9) { cols = 3; rows = 3; }
            else if (n <= 12){ cols = 4; rows = 3; }
            else             { cols = 4; rows = 4; }
            break;
        case 1: cols = 1; rows = n; break;
        case 2: cols = 2; rows = (n + 1) / 2; break;
        case 3: cols = 3; rows = (n + 2) / 3; break;
        case 4: cols = 2; rows = 2; break;
        case 6: cols = 3; rows = (n + 1) / 2; break;
        default: break;
    }
    cols = qMin(cols, n);

    // 1 个投屏:铺满整窗(无边距无间隙);多路:细间隔;独占全屏时同样铺满
    const bool fullBleed = (n == 1) || (m_focusView != nullptr);

    // 当前应显示的视图序列(排除焦点外与超出解码上限的会话)
    QList<SessionView *> shown;
    for (SessionView *view : active) {
        if (m_focusView && view != m_focusView)
            continue;
        if (shown.size() >= maxG)
            continue;
        shown.append(view);
    }

    // 单路铺满时:视频填满整窗(居中裁剪, 无黑边);多路网格则等比留边
    for (SessionView *view : m_views)
        view->setFillMode(fullBleed && shown.size() == 1 && shown.contains(view));

    // ---- 快速路径:布局无变化则跳过, 避免切换时重建容器导致闪烁 ----
    if (m_lastShown == shown && m_lastCols == cols && m_lastRows == rows
        && m_lastFullBleed == fullBleed && m_lastEmpty == active.isEmpty()) {
        return;
    }
    m_lastShown = shown;
    m_lastCols  = cols;
    m_lastRows  = rows;
    m_lastFullBleed = fullBleed;
    m_lastEmpty = active.isEmpty();

    // 只删除布局项, 不 reparent 视图 —— setParent(nullptr) 会重建嵌入的
    // d3d11 原生窗口容器, 是切换/重排时画面闪烁的根源
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;

    m_canvasLayout->setContentsMargins(fullBleed ? 0 : 12, fullBleed ? 0 : 12,
                                       fullBleed ? 0 : 12, fullBleed ? 0 : 12);
    m_grid->setSpacing(fullBleed ? 0 : 4);

    if (active.isEmpty()) {
        if (m_emptyCard) {
            m_emptyCard->show();
            m_grid->addWidget(m_emptyCard, 0, 0);
        }
        emit statusMessage(QStringLiteral("无会话"));
        emit sessionCountChanged(0);
        return;
    }

    if (m_emptyCard)
        m_emptyCard->hide();

    int row = 0, col = 0;
    for (SessionView *view : shown) {
        m_grid->addWidget(view, row, col);
        view->show();
        if (++col >= cols) { col = 0; ++row; }
    }
    // 隐藏未展示的会话(独占全屏外的 / 超出解码上限的)
    for (SessionView *view : m_views) {
        if (!shown.contains(view))
            view->hide();
    }
    emit statusMessage(QStringLiteral("会话数: %1  ·  布局: %2 路%3")
                           .arg(active.size())
                           .arg(m_layoutMode ? m_layoutMode : qMax(1, cols))
                           .arg(shown.size() < active.size()
                                    ? QStringLiteral("  ·  超出解码上限, 仅显示 %1 路").arg(shown.size())
                                    : QString()));
    emit sessionCountChanged(active.size());
}

void DesktopWindow::onViewFullscreen(const QString &sessionId)
{
    SessionView *target = nullptr;
    for (SessionView *v : m_views) {
        if (v->sessionId() == sessionId) { target = v; break; }
    }
    if (!target)
        return;

    if (m_focusView == target) {
        // 还原:恢复自动布局
        m_focusView = nullptr;
        if (target)
            target->setFullscreenActive(false);
    } else {
        m_focusView = target;
        target->setFullscreenActive(true);
    }
    relayout();
}

void DesktopWindow::startAirPlay()
{
    if (m_gatewayStarted)
        return;
    m_gatewayStarted = true;

    // 网关调度:唯一广播名 MirrorCenter, 内部按需启动静默 uxplay 实例,
    // 设备断开 30s 回收, 30s 内重连续用原实例(由 SDK 网关层保证)。
    mirror_gateway_callbacks_t cbs;
    cbs.on_log                = &DesktopWindow::gatewayLogCallback;
    cbs.on_client_connected   = &DesktopWindow::gatewayClientConnectedCallback;
    cbs.on_client_disconnected = &DesktopWindow::gatewayClientDisconnectedCallback;
    cbs.on_client_info        = &DesktopWindow::gatewayClientInfoCallback;
    cbs.on_decoder            = &DesktopWindow::gatewayDecoderCallback;

    const mirror_result_t rc = mirror_start_airplay_gateway(nullptr, nullptr,
                                                            nullptr, nullptr,
                                                            &cbs, this);
    if (rc != MIRROR_OK) {
        m_gatewayStarted = false;
        emit statusMessage(QStringLiteral("AirPlay 网关启动失败: %1")
                               .arg(QString::fromUtf8(mirror_last_error())));
        return;
    }
    emit statusMessage(QStringLiteral("AirPlay 接收已启动(MirrorCenter, 多设备自动调度)"));
}

void DesktopWindow::startMiracast()
{
    if (m_miracastStarted) return;
    m_miracastStarted = true;

    // 4 路 Miracast 槽位:Windows.Media.Miracast 原生支持多路同时连接,
    // 单服务进程承载全部连接, 每路独立帧端口。实际并发路数受网卡驱动硬件
    // 上限(MiracastReceiverStatus.MaxSimultaneousConnections)约束。
    constexpr int kMiracastSlots = 4;
    mirror_session_t *sessions[8] = {};
    int count = 0;
    const mirror_result_t rc = mirror_start_miracast_group(
        kMiracastSlots, "MirrorCenter", nullptr, sessions, &count);
    if (rc != MIRROR_OK || count <= 0) {
        m_miracastStarted = false;
        emit statusMessage(QStringLiteral("Miracast 启动失败: %1")
                               .arg(QString::fromUtf8(mirror_last_error())));
        return;
    }

    for (int i = 0; i < count; ++i) {
        const QString name = QStringLiteral("Miracast-%1").arg(i + 1);
        // deferStart=true:会话由 SDK 组 API 创建, 视图只接管句柄(adoptManualSession)
        auto *view = new SessionView(name, MIRROR_BACKEND_MIRACAST, m_canvasInner,
                                     /* deferStart */ true);
        connect(view, &SessionView::sessionClosed,
                this, &DesktopWindow::onSessionClosed);
        connect(view, &SessionView::fullscreenRequested,
                this, &DesktopWindow::onViewFullscreen);
        // 首帧前保持隐藏(Miracast 占位会话);收到首帧才在主窗口/列表出现
        connect(view, &SessionView::firstFrameReceived, this, [this]() {
            relayout();
            emit sourcesChanged();
        });
        view->adoptManualSession(sessions[i], name);
        m_views.append(view);
    }
    emit statusMessage(QStringLiteral("Miracast 接收已启动(%1 路槽位)").arg(count));
    relayout();
    emit sourcesChanged();
}

void DesktopWindow::startMiceBackend()
{
    if (m_miceStarted)
        return;
    m_miceStarted = true;

    // MS-MICE(Win+K 基础设施投屏):mDNS 发布 _display._tcp(container_id),
    // Windows 发送端自动发现并连入;媒体走局域网/有线, 多路不受 Wi-Fi
    // Direct 硬件上限约束, 普通网卡即可承载。
    mirror_gateway_callbacks_t cbs;
    cbs.on_log                = &DesktopWindow::gatewayLogCallback;
    cbs.on_client_connected   = &DesktopWindow::gatewayClientConnectedCallback;
    cbs.on_client_disconnected = &DesktopWindow::gatewayClientDisconnectedCallback;
    cbs.on_client_info        = &DesktopWindow::gatewayClientInfoCallback;
    cbs.on_decoder            = &DesktopWindow::gatewayDecoderCallback;

    const mirror_result_t rc = mirror_start_mice_backend("MirrorCenter", &cbs, this);
    if (rc != MIRROR_OK) {
        m_miceStarted = false;
        emit statusMessage(QStringLiteral("MS-MICE 接收端启动失败: %1")
                               .arg(QString::fromUtf8(mirror_last_error())));
        return;
    }
    emit statusMessage(QStringLiteral("MS-MICE 接收已启动(笔记本 Win+K 可搜到 MirrorCenter)"));
}

SessionView *DesktopWindow::addSession(const QString &name, mirror_backend_t backend)
{
    auto *view = new SessionView(name, backend, m_canvasInner);
    connect(view, &SessionView::sessionClosed,
            this, &DesktopWindow::onSessionClosed);
    connect(view, &SessionView::fullscreenRequested,
            this, &DesktopWindow::onViewFullscreen);
    // 首帧前保持隐藏(Miracast 占位会话);收到首帧才在主窗口/列表出现
    connect(view, &SessionView::firstFrameReceived, this, [this]() {
        relayout();
        emit sourcesChanged();
    });
    m_views.append(view);
    relayout();
    emit sourcesChanged();
    return view;
}

QList<SourceItem> DesktopWindow::sourceItems() const
{
    QList<SourceItem> items;
    for (SessionView *view : m_views) {
        if (!view || !view->isActive())
            continue;   // 未出画的占位会话不出现在列表
        SourceItem it;
        it.sessionId = view->sessionId();
        it.name    = view->deviceName();
        it.ip      = view->clientIp();
        it.backend = view->backend();
        it.status  = view->isRunning() ? QStringLiteral("投屏中")
                                       : QStringLiteral("连接中");
        items.append(it);
    }
    return items;
}

QPixmap DesktopWindow::thumbnailFor(const QString &sessionId) const
{
    for (SessionView *view : m_views) {
        if (view && view->sessionId() == sessionId)
            return view->thumbnail();
    }
    return QPixmap();
}

int DesktopWindow::maxGrid() const
{
    // 软解码(avdec_*):CPU 解码, 4 格封顶;其余(硬解) 16 格
    if (m_decoder.startsWith(QStringLiteral("avdec")))
        return 4;
    return 16;
}

void DesktopWindow::focusSession(const QString &sessionId)
{
    if (m_focusView) {
        // 先退出独占全屏, 恢复多窗口布局
        m_focusView->setFullscreenActive(false);
        m_focusView = nullptr;
    }
    for (int i = 0; i < m_views.size(); ++i) {
        if (m_views[i] && m_views[i]->sessionId() == sessionId) {
            SessionView *v = m_views.takeAt(i);
            m_views.prepend(v);   // 移到首位
            relayout();
            return;
        }
    }
}

/* ---- 网关回调(SDK 事件线程) ---- */

void DesktopWindow::gatewayLogCallback(const char *message, void *userdata)
{
    auto *self = static_cast<DesktopWindow *>(userdata);
    if (self && message)
        qInfo() << "[gateway]" << message;
}

void DesktopWindow::gatewayClientConnectedCallback(mirror_session_t *session,
                                                   const char *client_ip,
                                                   void *userdata)
{
    auto *self = static_cast<DesktopWindow *>(userdata);
    if (!self || !session)
        return;
    const QString ip = client_ip ? QString::fromUtf8(client_ip) : QString();
    // 句柄此刻仍由 SDK 持有;若在视图创建前设备就断开, 句柄会失效,
    // onGatewayClientConnected 里用 mirror_get_state 兜底判断。
    QMetaObject::invokeMethod(self, [self, session, ip]() {
        self->onGatewayClientConnected(session, ip);
    }, Qt::QueuedConnection);
}

void DesktopWindow::gatewayClientDisconnectedCallback(mirror_session_t *session,
                                                      const char *client_ip,
                                                      void *userdata)
{
    auto *self = static_cast<DesktopWindow *>(userdata);
    if (!self || !session)
        return;

    // 句柄在回调返回后即被 SDK 回收, 这里先同步取出视图(指针有效期内),
    // 再排队到主线程做 UI 清理, 避免使用已释放的句柄。
    SessionView *view = nullptr;
    {
        QMutexLocker locker(&self->m_gatewayMutex);
        view = self->m_gatewayViews.value(session, nullptr);
        self->m_gatewayViews.remove(session);
    }
    Q_UNUSED(client_ip)

    if (view) {
        QMetaObject::invokeMethod(view, [view]() {
            view->detach();
            emit view->sessionClosed(view->sessionId());
        }, Qt::QueuedConnection);
    }
}

// 来源手机信息就绪:事件线程回调 → 排队到主线程更新视图悬浮标签
void DesktopWindow::gatewayClientInfoCallback(mirror_session_t *session,
                                              const char *client_name,
                                              const char *client_model,
                                              void *userdata)
{
    auto *self = static_cast<DesktopWindow *>(userdata);
    if (!self || !session)
        return;
    const QString name = QString::fromUtf8(client_name ? client_name : "");
    const QString model = QString::fromUtf8(client_model ? client_model : "");
    // 仅用句柄作 map 键查找, 不 deref; 视图若已移除则为空, 忽略即可
    QMetaObject::invokeMethod(self, [self, session, name, model]() {
        SessionView *view = nullptr;
        {
            QMutexLocker locker(&self->m_gatewayMutex);
            view = self->m_gatewayViews.value(session, nullptr);
        }
        if (view)
            view->setClientInfo(name, model);
        emit self->sourcesChanged();   // 手机名就绪 → 刷新控制台列表
    }, Qt::QueuedConnection);
}

// 实例实际视频解码器就绪:软/硬解能力 → 更新最大宫格并重排
void DesktopWindow::gatewayDecoderCallback(mirror_session_t *session,
                                           const char *decoder,
                                           void *userdata)
{
    auto *self = static_cast<DesktopWindow *>(userdata);
    if (!self)
        return;
    const QString dec = QString::fromUtf8(decoder ? decoder : "");
    Q_UNUSED(session)
    QMetaObject::invokeMethod(self, [self, dec]() {
        if (self->m_decoder == dec)
            return;
        self->m_decoder = dec;
        const int g = self->maxGrid();
        emit self->statusMessage(QStringLiteral("视频解码器: %1(最大 %2 格)")
                                     .arg(dec.isEmpty() ? QStringLiteral("未知") : dec)
                                     .arg(g));
        self->relayout();
    }, Qt::QueuedConnection);
}

void DesktopWindow::onGatewayClientConnected(mirror_session_t *session, const QString &ip)
{
    if (!session)
        return;
    // 兜底:句柄已失效(设备在视图创建前断开)则忽略
    if (mirror_get_state(session) == MIRROR_STATE_CLOSED)
        return;

    // MS-MICE 会话由 SDK 以 "MICE:" 前缀标记, 显示 Source 友好名(而非 IP)
    const bool isMice = ip.startsWith(QStringLiteral("MICE:"));
    const QString displayName = isMice ? ip.mid(5) : ip;

    auto *view = new SessionView(displayName,
                                 isMice ? MIRROR_BACKEND_MICE : MIRROR_BACKEND_AIRPLAY,
                                 m_canvasInner);
    connect(view, &SessionView::sessionClosed,
            this, &DesktopWindow::onSessionClosed);
    connect(view, &SessionView::fullscreenRequested,
            this, &DesktopWindow::onViewFullscreen);

    view->adoptGatewaySession(session, displayName);
    m_views.append(view);
    {
        QMutexLocker locker(&m_gatewayMutex);
        m_gatewayViews.insert(session, view);
    }
    relayout();
    emit statusMessage(isMice
                           ? QStringLiteral("Windows 设备 %1 已连入").arg(displayName)
                           : QStringLiteral("设备 %1 已连入").arg(ip));
    emit sourcesChanged();
}

void DesktopWindow::onGatewayClientDisconnected(mirror_session_t *session)
{
    Q_UNUSED(session)
    // 视图清理已在 gatewayClientDisconnectedCallback 中排队执行
}

void DesktopWindow::onSessionClosed(const QString &sessionId)
{
    // 找出触发关闭的视图
    SessionView *closed = nullptr;
    for (SessionView *v : m_views) {
        if (v->sessionId() == sessionId) { closed = v; break; }
    }
    if (!closed)
        return;

    // Miracast 组:所有路共享同一服务进程(任一路断开会整组失效),
    // 关闭任一路时整组一起关闭。
    const bool closeAllMiracast = closed->deviceName().startsWith(QStringLiteral("Miracast"));

    QList<SessionView *> toRemove;
    for (SessionView *v : m_views) {
        if (v == closed)
            toRemove.append(v);
        else if (closeAllMiracast && v->deviceName().startsWith(QStringLiteral("Miracast")))
            toRemove.append(v);
    }

    for (SessionView *view : toRemove) {
        const bool wasFocus = (m_focusView == view);
        // 从网关视图表移除(若存在)
        {
            QMutexLocker locker(&m_gatewayMutex);
            for (auto it = m_gatewayViews.begin(); it != m_gatewayViews.end(); ) {
                if (it.value() == view)
                    it = m_gatewayViews.erase(it);
                else
                    ++it;
            }
        }
        m_views.removeOne(view);
        view->stop();
        view->deleteLater();
        if (wasFocus)
            m_focusView = nullptr;
    }

    // 已无 Miracast 视图 → 允许重新启动整组
    bool hasMira = false;
    for (SessionView *v : m_views)
        if (v->deviceName().startsWith(QStringLiteral("Miracast"))) { hasMira = true; break; }
    if (!hasMira)
        m_miracastStarted = false;

    // 已无 MS-MICE 会话 → 允许重新启动接收端
    bool hasMice = false;
    for (SessionView *v : m_views)
        if (v->backend() == MIRROR_BACKEND_MICE) { hasMice = true; break; }
    if (!hasMice)
        m_miceStarted = false;

    relayout();
    emit sourcesChanged();
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
