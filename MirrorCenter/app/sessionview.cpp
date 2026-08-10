#include "sessionview.h"

#include <QLabel>
#include <QWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUuid>
#include <QDebug>
#include <QMetaObject>
#include <QPixmap>

SessionView::SessionView(const QString &deviceName, mirror_backend_t backend, QWidget *parent)
    : QWidget(parent)
    , m_deviceName(deviceName)
    , m_backend(backend)
{
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setObjectName("sessionCard");

    // 顶栏:设备名 + 状态指示点 + 状态文字
    m_topBar = new QWidget(this);
    m_topBar->setObjectName("sessionTopBar");
    auto *barLayout = new QHBoxLayout(m_topBar);
    barLayout->setContentsMargins(14, 8, 14, 8);
    barLayout->setSpacing(8);

    // 设备名前面:小图标(根据后端区分)
    QString prefixIcon = (m_backend == MIRROR_BACKEND_AIRPLAY)
                            ? QStringLiteral("🍎")
                            : QStringLiteral("📡");
    auto *prefixLabel = new QLabel(prefixIcon, m_topBar);
    prefixLabel->setStyleSheet(
        "background-color:transparent; font-size:14px;");
    barLayout->addWidget(prefixLabel);

    auto *nameLabel = new QLabel(m_deviceName, m_topBar);
    nameLabel->setObjectName("sessionTitle");
    barLayout->addWidget(nameLabel);
    barLayout->addStretch();

    // 状态指示点(● 灰 / 绿)
    m_statusDot = new QLabel(QStringLiteral("●"), m_topBar);
    m_statusDot->setStyleSheet(
        "color:#6B7488; font-size:10px; background-color:transparent;");
    barLayout->addWidget(m_statusDot);

    m_statusLabel = new QLabel(QStringLiteral("启动中..."), m_topBar);
    m_statusLabel->setObjectName("sessionStatus");
    barLayout->addWidget(m_statusLabel);
    layout->addWidget(m_topBar);

    // 占位区:AirPlay 拿到句柄后替换为嵌入窗口;Miracast 为帧显示区
    m_placeholder = new QWidget(this);
    auto *phLayout = new QVBoxLayout(m_placeholder);
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
    auto *phLabel = new QLabel(placeholderText, m_placeholder);
    phLabel->setAlignment(Qt::AlignCenter);
    phLabel->setObjectName("placeholderText");
    phLayout->addWidget(phLabel);
    layout->addWidget(m_placeholder, 1);

    if (m_backend == MIRROR_BACKEND_MIRACAST) {
        // Miracast 帧模式:用 QLabel 显示最新帧
        m_videoLabel = new QLabel(this);
        m_videoLabel->setAlignment(Qt::AlignCenter);
        m_videoLabel->setMinimumSize(320, 200);
        m_videoLabel->setScaledContents(false);
        layout->replaceWidget(m_placeholder, m_videoLabel);
        m_placeholder->deleteLater();
        m_placeholder = nullptr;
        m_videoLabel->show();
    }

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

SessionView::~SessionView()
{
    stop();
}

void SessionView::stop()
{
    if (m_sdkSession) {
        mirror_destroy_session(m_sdkSession);
        m_sdkSession = nullptr;
    }
    m_running = false;
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
    QMetaObject::invokeMethod(self, [self]() {
        self->renderFrame();
    }, Qt::QueuedConnection);
    Q_UNUSED(session)
}

/* ---- UI 层 ---- */

void SessionView::renderFrame()
{
    if (!m_sdkSession || !m_videoLabel)
        return;
    mirror_frame_t frame;
    if (mirror_get_frame(m_sdkSession, &frame) != MIRROR_OK)
        return;

    // data 仅在调用期间有效:先拷贝成 QImage
    QImage img(frame.width, frame.height, QImage::Format_ARGB32_Premultiplied);
    const int copyBytes = qMin(frame.stride, img.bytesPerLine());
    for (int y = 0; y < frame.height; ++y) {
        memcpy(img.scanLine(y),
               frame.data + static_cast<qint64>(y) * frame.stride,
               copyBytes);
    }
    m_videoLabel->setPixmap(QPixmap::fromImage(img).scaled(
        m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    setStatus(QStringLiteral("接收中 %1x%2").arg(frame.width).arg(frame.height));
}

void SessionView::attachWindow(qulonglong wid)
{
    if (!wid)
        return;

    QWindow *native = QWindow::fromWinId(WId(wid));
    if (!native)
        return;

    m_childWindow = native;
    native->setFlags(native->flags() | Qt::ForeignWindow);

    QWidget *container = QWidget::createWindowContainer(native, this);
    container->setFocusPolicy(Qt::StrongFocus);

    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    if (layout && m_placeholder) {
        layout->removeWidget(m_placeholder);
        m_placeholder->deleteLater();
        m_placeholder = nullptr;
    }
    if (layout)
        layout->addWidget(container, 1);

    setStatus(QStringLiteral("已连接"));
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
