#include "sessionview.h"
#include "audiocontrol.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <QLabel>
#include <QToolButton>
#include <QWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUuid>
#include <QDebug>
#include <QDateTime>
#include <QMetaObject>
#include <QPixmap>

SessionView::SessionView(const QString &deviceName, mirror_backend_t backend,
                         QWidget *parent, bool deferStart)
    : QWidget(parent)
    , m_deviceName(deviceName)
    , m_backend(backend)
{
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    buildUi();

    // 缩略图节拍:画面有变化时 1.2s 抓一次, 静止时 3s(去重, 不一直切图)
    m_thumbTimer.setInterval(1200);
    connect(&m_thumbTimer, &QTimer::timeout, this, [this]() {
        const QImage img = captureThumbnail();
        if (img.isNull())
            return;
        if (thumbChanged(img)) {
            m_lastThumb = QPixmap::fromImage(img);
            m_thumbTimer.setInterval(1200);
            emit thumbnailUpdated();
        } else {
            // 画面无变化:延长抓取间隔, 降低 CPU/GDI 开销
            m_thumbTimer.setInterval(3000);
        }
    });

    // 无线链路速率查询:周期差分 WMI 性能计数器(Wifi 网卡累计字节),
    // 得到源网络实际接收/发送速率(Mbps)
    m_netTimer.setInterval(2500);
    connect(&m_netTimer, &QTimer::timeout, this, [this]() { queryNetRate(); });

    // Miracast 仍走"自建会话"流程;AirPlay 由网关回调 adoptGatewaySession 包装。
    // deferStart=true 时(多路 Miracast 组)由 DesktopWindow 调用 adoptManualSession 接管。
    if (m_backend == MIRROR_BACKEND_MIRACAST && !deferStart)
        startStandaloneSession();
}

void SessionView::buildUi()
{
    // 无边框、视频铺满整格;设备信息以半透明悬浮标签叠加在画面左上角
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setObjectName("sessionCard");

    // 视频区:占位 → 嵌入窗口 / Miracast 帧
    m_videoArea = new QWidget(this);
    auto *phLayout = new QVBoxLayout(m_videoArea);
    phLayout->setContentsMargins(0, 0, 0, 0);

    QString placeholderText;
    if (m_backend == MIRROR_BACKEND_AIRPLAY) {
        placeholderText = QStringLiteral(
            "等待 iPhone / iPad 投屏连接……\n\n"
            "① 手机与电脑连接同一 Wi-Fi\n"
            "② 控制中心 → 屏幕镜像\n"
            "③ 选择「%1」")
            .arg(m_deviceName);
    } else {
        placeholderText = QStringLiteral(
            "等待安卓 / 笔记本投屏连接……\n\n"
            "① 设备与电脑连接同一 Wi-Fi\n"
            "② 安卓:设置 → 无线投屏\n"
            "   或 Windows:Win+K → 连接无线显示器\n"
            "③ 选择「%1」")
            .arg(m_deviceName);
    }
    auto *phLabel = new QLabel(placeholderText, m_videoArea);
    phLabel->setAlignment(Qt::AlignCenter);
    phLabel->setObjectName("placeholderText");
    phLayout->addWidget(phLabel);
    layout->addWidget(m_videoArea, 0, 0);

    if (m_backend == MIRROR_BACKEND_MIRACAST) {
        // Miracast 帧模式:用 QLabel 显示最新帧(铺满整格)
        m_videoLabel = new QLabel(this);
        m_videoLabel->setAlignment(Qt::AlignCenter);
        m_videoLabel->setScaledContents(false);
        layout->removeWidget(m_videoArea);
        m_videoArea->deleteLater();
        m_videoArea = nullptr;
        layout->addWidget(m_videoLabel, 0, 0);
    }

    // 悬浮信息标签:独立顶层无边框小窗, 悬浮在画面左上角。
    // 必须用顶层窗: 嵌入的 D3D11 视频是原生 HWND, 会盖住普通 Qt 子控件。
    m_infoBadge = new QWidget;
    m_infoBadge->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool
                                | Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint);
    m_infoBadge->setAttribute(Qt::WA_ShowWithoutActivating);
    m_infoBadge->setStyleSheet(
        "QWidget#infoBadge {"
        "  color:#FFFFFF; font-size:12px; font-weight:600;"
        "  background-color:rgba(10, 14, 24, 180);"
        "  border-radius:14px;"
        "}");
    m_infoBadge->setObjectName("infoBadge");

    m_statusDot = new QLabel(QStringLiteral("●"), m_infoBadge);
    m_statusDot->setStyleSheet("color:#6B7488; font-size:10px;"
                               "background-color:transparent;");
    m_statusLabel = new QLabel(QStringLiteral("启动中..."), m_infoBadge);
    m_statusLabel->setStyleSheet("background-color:transparent;");

    m_rateLabel = new QLabel(QStringLiteral("0 Mbps"), m_infoBadge);
    m_rateLabel->setStyleSheet("background-color:transparent;");

    m_devLabel = new QLabel(m_deviceName, m_infoBadge);
    m_devLabel->setStyleSheet("background-color:transparent;");

    auto *badgeL = new QHBoxLayout(m_infoBadge);
    badgeL->setContentsMargins(12, 6, 8, 6);
    badgeL->setSpacing(6);
    badgeL->addWidget(m_devLabel);
    badgeL->addWidget(m_statusDot);
    badgeL->addWidget(m_statusLabel);
    badgeL->addWidget(m_rateLabel);

    // 静音 / 全屏按钮
    const QString btnQss =
        "QToolButton { color:#C8D0DC; background:transparent; border:none;"
        "              font-size:14px; padding:2px 4px; }"
        "QToolButton:hover { color:#FFFFFF; }"
        "QToolButton:pressed { color:#4ADE80; }";
    m_muteBtn = new QToolButton(m_infoBadge);
    m_muteBtn->setText(QStringLiteral("🔊"));
    m_muteBtn->setToolTip(QStringLiteral("静音 / 取消静音"));
    m_muteBtn->setCursor(Qt::PointingHandCursor);
    m_muteBtn->setStyleSheet(btnQss);
    connect(m_muteBtn, &QToolButton::clicked, this, &SessionView::toggleMute);
    badgeL->addWidget(m_muteBtn);

    m_fullBtn = new QToolButton(m_infoBadge);
    m_fullBtn->setText(QStringLiteral("⛶"));
    m_fullBtn->setToolTip(QStringLiteral("全屏 / 还原"));
    m_fullBtn->setCursor(Qt::PointingHandCursor);
    m_fullBtn->setStyleSheet(btnQss);
    connect(m_fullBtn, &QToolButton::clicked, this, &SessionView::toggleFullscreen);
    badgeL->addWidget(m_fullBtn);
}

void SessionView::startStandaloneSession()
{
    // 通过 SDK 启动会话
    mirror_callbacks_t cbs;
    cbs.on_state  = &SessionView::onStateCallback;
    cbs.on_window = &SessionView::onWindowCallback;
    cbs.on_log    = &SessionView::onLogCallback;
    cbs.on_frame  = &SessionView::onFrameCallback;

    mirror_session_t *session = nullptr;
    const QByteArray nameUtf8 = m_deviceName.toUtf8();
    qInfo() << "[view] calling mirror_start_session";
    const mirror_result_t rc = mirror_start_session(m_backend,
                                                    nameUtf8.constData(),
                                                    nullptr, /* exe 由 SDK 搜索 */
                                                    nullptr, /* args */
                                                    &cbs,
                                                    this,    /* userdata = this */
                                                    &session);
    qInfo() << "[view] mirror_start_session rc=" << int(rc)
            << "session=" << (session ? "non-null" : "null");
    if (rc != MIRROR_OK) {
        setStatus(QStringLiteral("SDK 启动失败: %1").arg(mirror_last_error()));
        m_running = false;
        return;
    }
    m_sdkSession = session;
    m_running = true;
}

void SessionView::attachCallbacks()
{
    if (!m_sdkSession)
        return;
    mirror_callbacks_t cbs;
    cbs.on_state  = &SessionView::onStateCallback;
    cbs.on_window = &SessionView::onWindowCallback;
    cbs.on_log    = &SessionView::onLogCallback;
    cbs.on_frame  = &SessionView::onFrameCallback;
    mirror_set_callbacks(m_sdkSession, &cbs, this);
}

void SessionView::adoptGatewaySession(mirror_session_t *sdkSession, const QString &clientIp)
{
    m_gatewayMode = true;
    m_clientIp = clientIp;
    m_sdkSession = sdkSession;
    m_running = true;

    attachCallbacks();
    setStatus(QStringLiteral("已连接 %1").arg(clientIp));

    // 网关的实例窗口在空闲时被隐藏, 且窗口可能早于连接就绪,
    // 这里主动查询一次, 有窗口立即嵌入, 否则等 on_window 回调。
    const uint64_t wid = mirror_get_window(sdkSession);
    if (wid)
        attachWindow(static_cast<qulonglong>(wid));
}

void SessionView::adoptManualSession(mirror_session_t *sdkSession, const QString &deviceName)
{
    m_deviceName = deviceName;
    if (m_devLabel)
        m_devLabel->setText(deviceName);
    m_sdkSession = sdkSession;
    m_running = true;

    // 帧/状态经 SDK 回调驱动(帧回调 → renderFrame → 首帧出画)
    attachCallbacks();
    setStatus(QStringLiteral("等待设备投屏连接……"));
}

SessionView::~SessionView()
{
    stop();
    if (m_infoBadge) {
        m_infoBadge->hide();
        m_infoBadge->deleteLater();
        m_infoBadge = nullptr;
    }
}

void SessionView::stop()
{
    m_thumbTimer.stop();
    m_netTimer.stop();
    m_lastThumb = QPixmap();
    m_thumbFp = QImage();
    m_hasThumbFp = false;
    if (m_gatewayMode) {
        // 网关模式:句柄由网关/设备生命周期管理, 只解除回调与嵌入
        if (m_sdkSession)
            mirror_set_callbacks(m_sdkSession, nullptr, nullptr);
        m_sdkSession = nullptr;
    } else if (m_sdkSession) {
        mirror_destroy_session(m_sdkSession);
        m_sdkSession = nullptr;
    }
    m_running = false;
}

void SessionView::detach()
{
    // 移除嵌入的视频窗口容器(视图随后由 DesktopWindow 删除)
    m_thumbTimer.stop();
    m_netTimer.stop();
    m_lastThumb = QPixmap();
    m_thumbFp = QImage();
    m_hasThumbFp = false;
    auto *layout = qobject_cast<QGridLayout *>(this->layout());
    if (layout && m_childWindow) {
        for (int i = layout->count() - 1; i >= 0; --i) {
            QLayoutItem *item = layout->itemAt(i);
            QWidget *w = item ? item->widget() : nullptr;
            if (w && w != m_infoBadge && w != m_videoArea) {
                layout->removeWidget(w);
                w->setParent(nullptr);
                w->deleteLater();
            }
        }
        m_childWindow = nullptr;
    }
    // 恢复占位提示区
    if (m_videoArea && m_videoArea->parent() != this) {
        if (auto *g = qobject_cast<QGridLayout *>(this->layout()))
            g->addWidget(m_videoArea, 0, 0);
        m_videoArea->show();
    }
    if (m_infoBadge)
        m_infoBadge->hide();
    m_running = false;
    setStatus(QStringLiteral("已断开"));
}

/* ---- SDK 回调(C 层,事件线程触发) ---- */

void SessionView::onStateCallback(mirror_session_t *session, mirror_state_t state, void *userdata)
{
    auto *self = static_cast<SessionView *>(userdata);
    if (!self)
        return;
    const QString text = (state == MIRROR_STATE_FAILED)
                             ? QStringLiteral("启动失败")
                             : (state == MIRROR_STATE_CLOSED)
                                   ? QStringLiteral("已关闭")
                                   : QStringLiteral("状态 %1").arg(int(state));
    QMetaObject::invokeMethod(self, [self, text]() {
        self->setStatus(text);
        if (text == QStringLiteral("已关闭"))
            emit self->sessionClosed(self->m_sessionId);
    }, Qt::QueuedConnection);
    Q_UNUSED(session)
}

void SessionView::onWindowCallback(mirror_session_t *session, uint64_t handle, void *userdata)
{
    auto *self = static_cast<SessionView *>(userdata);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self, handle]() {
        self->attachWindow(static_cast<qulonglong>(handle));
    }, Qt::QueuedConnection);
    Q_UNUSED(session)
}

void SessionView::onLogCallback(mirror_session_t *session, const char *message, void *userdata)
{
    auto *self = static_cast<SessionView *>(userdata);
    if (self)
        qInfo() << "[mirrorsdk:" << self->m_deviceName << "]" << (message ? message : "");
    Q_UNUSED(session)
}

void SessionView::onFrameCallback(mirror_session_t *session, void *userdata)
{
    auto *self = static_cast<SessionView *>(userdata);
    if (!self)
        return;
    // 节流:上一帧还没渲染完就丢弃本次回调, 避免 UI 事件积压导致卡顿/延迟
    if (self->m_framePending)
        return;
    self->m_framePending = true;
    QMetaObject::invokeMethod(self, [self]() {
        self->renderFrame();
    }, Qt::QueuedConnection);
    Q_UNUSED(session)
}

/* ---- UI 层 ---- */

QPixmap SessionView::thumbnail() const
{
    return m_lastThumb;
}

bool SessionView::isActive() const
{
    return m_gatewayMode || m_childWindow || m_hasFirstFrame;
}

QImage SessionView::captureThumbnail()
{
#ifdef _WIN32
    // 嵌入的原生渲染窗口(AirPlay d3d11videosink / Miracast swap chain):
    // 抓取实际渲染内容用作缩略图
    if (m_childWindow) {
        HWND hwnd = reinterpret_cast<HWND>(m_childWindow->winId());
        if (!hwnd || !::IsWindow(hwnd))
            return QImage();

        RECT rc;
        if (!::GetClientRect(hwnd, &rc))
            return QImage();
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
            return QImage();

        HDC winDc = ::GetDC(hwnd);
        if (!winDc)
            return QImage();
        HDC memDc = ::CreateCompatibleDC(winDc);
        HBITMAP bmp = ::CreateCompatibleBitmap(winDc, w, h);
        HGDIOBJ old = ::SelectObject(memDc, bmp);

        // PW_RENDERFULLCONTENT:抓取 D3D11 实际渲染内容(而非窗口表面)
        BOOL ok = ::PrintWindow(hwnd, memDc, PW_RENDERFULLCONTENT);
        if (!ok)   // 回退普通 BitBlt
            ok = ::BitBlt(memDc, 0, 0, w, h, winDc, 0, 0, SRCCOPY);

        ::SelectObject(memDc, old);

        QImage img;
        if (ok) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;   // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
            if (::GetDIBits(memDc, bmp, 0, h, img.bits(), &bmi, DIB_RGB_COLORS) == 0)
                img = QImage();
        }
        ::DeleteObject(bmp);
        ::DeleteDC(memDc);
        ::ReleaseDC(hwnd, winDc);
        return img;
    }
#endif
    // Miracast:帧模式,直接取最近显示的画面
    if (m_videoLabel) {
        QPixmap p = m_videoLabel->pixmap();
        if (!p.isNull())
            return p.toImage();
    }
    return QImage();
}

bool SessionView::thumbChanged(const QImage &thumb)
{
    // 缩成 16x9 指纹(忽略宽高比差异, 仅比较内容), 成本极低
    const QImage fp = thumb.scaled(16, 9, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (!m_hasThumbFp || fp.size() != m_thumbFp.size()) {
        m_thumbFp = fp;
        m_hasThumbFp = true;
        return true;
    }
    if (fp == m_thumbFp)   // QImage operator== 逐像素比较(16x9=144 像素)
        return false;

    // 统计差异像素比例, 超过 1% 视为画面有实际变化
    int diff = 0;
    const int total = fp.width() * fp.height();
    for (int y = 0; y < fp.height(); ++y) {
        for (int x = 0; x < fp.width(); ++x) {
            const QRgb a = fp.pixel(x, y);
            const QRgb b = m_thumbFp.pixel(x, y);
            if (qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                + qAbs(qBlue(a) - qBlue(b)) > 24)
                ++diff;
        }
    }
    m_thumbFp = fp;
    return (double(diff) / total) > 0.01;
}

void SessionView::setFillMode(bool on)
{
    m_fillMode = on;
}

void SessionView::renderFrame()
{
    // 允许下一个帧回调排队(节流:UI 渲染期间丢弃新帧)
    m_framePending = false;
    if (!m_sdkSession || !m_videoLabel)
        return;
    mirror_frame_t frame;
    if (mirror_get_frame(m_sdkSession, &frame) != MIRROR_OK) {
        qWarning() << "[view] mirror_get_frame failed";
        return;
    }

    // data 仅在调用期间有效:先拷贝成 QImage
    QImage img(frame.width, frame.height, QImage::Format_RGB32);
    const int copyBytes = qMin(frame.stride, img.bytesPerLine());
    for (int y = 0; y < frame.height; ++y) {
        memcpy(img.scanLine(y),
               frame.data + static_cast<qint64>(y) * frame.stride,
               copyBytes);
    }
    // 铺满模式:等比放大到覆盖整个目标区后居中裁剪(无黑边, 不变形);
    // 否则等比缩放到区域内(保留黑边)。
    static auto scaledToCover = [](const QImage &src, const QSize &target) {
        if (target.isEmpty())
            return src;
        const double s = qMax(double(target.width()) / src.width(),
                              double(target.height()) / src.height());
        const QSize sz(qMax(1, qRound(src.width() * s)), qMax(1, qRound(src.height() * s)));
        const QImage big = src.scaled(sz, Qt::KeepAspectRatio, Qt::FastTransformation);
        const int dx = (big.width() - target.width()) / 2;
        const int dy = (big.height() - target.height()) / 2;
        return big.copy(dx, dy, target.width(), target.height());
    };
    const QImage scaledImg = m_fillMode
        ? scaledToCover(img, m_videoLabel->size())
        : img.scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation);
    m_videoLabel->setPixmap(QPixmap::fromImage(scaledImg));
    setStatus(QStringLiteral("接收中 %1x%2").arg(frame.width).arg(frame.height));

    // 首帧:占位会话真正出画, 通知主窗口/列表开始显示
    if (!m_hasFirstFrame) {
        m_hasFirstFrame = true;
        emit firstFrameReceived();
    }

    // 帧率/分辨率统计(1s 窗口):附带显示在无线链路速率旁
    ++m_rateFrames;
    m_lastW = frame.width;
    m_lastH = frame.height;
    if (!m_rateTimer.isValid()) {
        m_rateTimer.start();
    } else if (m_rateTimer.elapsed() >= 1000) {
        m_lastFps = m_rateFrames;
        m_rateFrames = 0;
        m_rateTimer.restart();
        // 显示帧率打点(供日志核对解码/渲染是否吃得住)
        qInfo() << "[fps]" << m_lastFps << "fps"
                << frame.width << "x" << frame.height;
    }
    // 无线链路速率:首次收到帧后启动周期查询
    if (!m_netTimer.isActive())
        m_netTimer.start();

    if (!m_thumbTimer.isActive())
        m_thumbTimer.start();
}

void SessionView::queryNetRate()
{
    // 上一次查询还在跑就跳过(避免进程堆积)
    if (m_netProc && m_netProc->state() != QProcess::NotRunning)
        return;

    m_netProc = new QProcess(this);
    connect(m_netProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        const QString out =
            QString::fromUtf8(m_netProc->readAllStandardOutput()).trimmed();
        m_netProc->deleteLater();
        m_netProc = nullptr;

        // 输出形如 "R=8398" "S=28732"(字节/秒, Get-Counter 实时值, 与任务管理器一致)
        double rxBps = -1, txBps = -1;
        const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("R=")))
                rxBps = line.mid(2).toDouble();
            else if (line.startsWith(QLatin1String("S=")))
                txBps = line.mid(2).toDouble();
        }
        if (rxBps < 0)
            return;
        if (txBps < 0)
            txBps = 0;
        const double rxMbps = rxBps * 8.0 / 1e6;
        const double txMbps = txBps * 8.0 / 1e6;

        // 显示:↓接收 ↑发送 Mbps + 帧率 + 分辨率
        QString s = QStringLiteral("↓%1 ↑%2 Mbps")
                        .arg(rxMbps, 0, 'f', 1)
                        .arg(txMbps, 0, 'f', 1);
        if (m_lastFps > 0)
            s += QStringLiteral(" · %1fps · %2×%3")
                     .arg(m_lastFps).arg(m_lastW).arg(m_lastH);
        m_rateLabel->setText(s);
    });

    m_netProc->start(
        QStringLiteral("powershell"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
         QStringLiteral("Get-Counter '\\Network Interface(*)\\Bytes Received/sec','\\Network Interface(*)\\Bytes Sent/sec' -ErrorAction Stop | "
                        "ForEach-Object {$_.CounterSamples | Where-Object {$_.InstanceName -match 'Wifi|WLAN'} | "
                        "ForEach-Object {if($_.Path -match 'Received'){'R='+[math]::Round($_.CookedValue)}else{'S='+[math]::Round($_.CookedValue)}}}")});
}

void SessionView::attachWindow(qulonglong wid)
{
    // 设备断开后 GStreamer/d3d11 会销毁渲染窗口, 重连/新设备可能拿到已失效句柄。
    // 无效句柄直接忽略, 等 on_window 回调(新窗口就绪)再嵌入, 避免 Qt ASSERT 崩溃。
    if (!wid || !::IsWindow(reinterpret_cast<HWND>(wid)))
        return;

    qInfo().nospace() << "[view] attachWindow wid=" << wid
        << " at " << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
        << " session=" << m_sessionId;

    QWindow *native = QWindow::fromWinId(WId(wid));
    if (!native)
        return;

    m_childWindow = native;
    native->setFlags(native->flags() | Qt::ForeignWindow);

    QWidget *container = QWidget::createWindowContainer(native, this);
    container->setFocusPolicy(Qt::StrongFocus);

    if (auto *g = qobject_cast<QGridLayout *>(this->layout())) {
        if (m_videoArea) {
            g->removeWidget(m_videoArea);
            m_videoArea->deleteLater();
            m_videoArea = nullptr;
        }
        // Miracast 帧模式:m_videoLabel 是占位 QLabel, swap chain 窗口嵌入后需移除
        if (m_videoLabel) {
            g->removeWidget(m_videoLabel);
            m_videoLabel->deleteLater();
            m_videoLabel = nullptr;
        }
        g->addWidget(container, 0, 0);
        container->show();
    }
    if (m_infoBadge)
        m_infoBadge->raise();

    setStatus(QStringLiteral("已连接"));
    if (!m_thumbTimer.isActive())
        m_thumbTimer.start();
}

void SessionView::updateInfoBadge()
{
    if (!m_infoBadge)
        return;
    if (!isVisible()) {
        m_infoBadge->hide();
        return;
    }
    // 定位到本视图左上角(全局坐标), 悬浮在视频之上
    m_infoBadge->move(mapToGlobal(QPoint(12, 12)));
    m_infoBadge->show();
}

void SessionView::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    updateInfoBadge();
}

void SessionView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateInfoBadge();
}

void SessionView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    updateInfoBadge();
    // 视图重新可见且仍有画面:恢复缩略图抓取
    if (m_running && m_sdkSession && !m_thumbTimer.isActive())
        m_thumbTimer.start();
}

void SessionView::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (m_infoBadge)
        m_infoBadge->hide();
    // 视图不可见(被覆盖/超限):暂停抓图, 省 CPU/GDI
    m_thumbTimer.stop();
}

void SessionView::toggleMute()
{
    if (!m_sdkSession)
        return;
    const uint64_t pid = mirror_get_process_id(m_sdkSession);
    if (!pid) {
        setStatus(QStringLiteral("后端未就绪"));
        return;
    }
    m_muted = !m_muted;
    if (!mirrorui::setProcessAudioMute(pid, m_muted)) {
        m_muted = !m_muted;   // 未找到音频会话, 回滚
        setStatus(QStringLiteral("静音控制失败"));
        return;
    }
    if (m_muteBtn)
        m_muteBtn->setText(m_muted ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    setStatus(m_muted ? QStringLiteral("已静音") : QStringLiteral("已取消静音"));
}

void SessionView::toggleFullscreen()
{
    emit fullscreenRequested(m_sessionId);
}

void SessionView::setFullscreenActive(bool active)
{
    if (m_fullBtn)
        m_fullBtn->setText(active ? QStringLiteral("❐") : QStringLiteral("⛶"));
}

void SessionView::setClientInfo(const QString &name, const QString &model)
{
    Q_UNUSED(model)   // 只显示设备名称
    if (!name.isEmpty() && m_devLabel)
        m_devLabel->setText(name);
}

void SessionView::setStatus(const QString &s)
{
    if (m_statusLabel)
        m_statusLabel->setText(s);
    if (m_statusDot) {
        // 根据状态切换指示点颜色
        QString color = "#6B7488";                      // 默认灰
        if (s.contains(QStringLiteral("已连接")) ||
            s.contains(QStringLiteral("接收中"))) {
            color = "#4ADE80";                          // 在线绿
        } else if (s.contains(QStringLiteral("失败")) ||
                   s.contains(QStringLiteral("关闭"))) {
            color = "#F87171";                          // 异常红
        }
        m_statusDot->setStyleSheet(QStringLiteral(
            "color:%1; font-size:10px; background-color:transparent;").arg(color));
    }
    emit statusChanged(m_sessionId, s);
}
