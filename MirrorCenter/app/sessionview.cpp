#ifdef _WIN32
// winsock2.h 必须先于一切(windows.h 默认引入 winsock.h, 后含 winsock2.h 会冲突);
// ws2ipdef.h 定义 _WS2IPDEF_, 否则 SDK 26100 的 netioapi.h 会跳过
// MIB_IF_ROW2/GetIfTable2/FreeMibTable 整段声明
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "sessionview.h"
#include "audiocontrol.h"

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
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLTexture>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLShaderProgram>

// ---- GLFrameSurface:GPU 纹理上传 + GPU 着色器缩放/裁切绘制 ----
GLFrameSurface::GLFrameSurface(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAutoFillBackground(false);
    // GL 上下文就绪前的首帧也保持黑底(避免启动瞬间白闪)
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0, 0, 0));
    setPalette(pal);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

GLFrameSurface::~GLFrameSurface()
{
    makeCurrent();
    delete m_tex;
    m_tex = nullptr;
    doneCurrent();
}

void GLFrameSurface::initializeGL()
{
    m_prog = new QOpenGLShaderProgram(this);
    // 顶点:纹理坐标 y 与图像一致(图像顶部 v0), 绘制时目标矩形顶部在 GL 上方
    const char *vsrc =
        "attribute vec2 aPos;"
        "attribute vec2 aUV;"
        "varying vec2 vUV;"
        "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char *fsrc =
        "uniform sampler2D uTex;"
        "varying vec2 vUV;"
        "void main(){ gl_FragColor = texture2D(uTex, vUV); }";
    if (!m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vsrc)
        || !m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fsrc)
        || !m_prog->link())
        qWarning() << "[GL] shader link failed:" << m_prog->log();
}

void GLFrameSurface::setFrame(const QImage &img, const QRectF &src, const QRectF &dst)
{
    m_src = src;
    m_dst = dst;
    if (img.isNull()) {
        update();
        return;
    }
    makeCurrent();
    if (!m_tex) {
        m_tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        m_tex->setMinificationFilter(QOpenGLTexture::Linear);
        m_tex->setMagnificationFilter(QOpenGLTexture::Linear);
        m_tex->setWrapMode(QOpenGLTexture::ClampToEdge);
    }
    if (m_tex->width() != img.width() || m_tex->height() != img.height()) {
        // 尺寸变化:全量分配+上传
        m_tex->destroy();
        m_tex->setData(img);
    } else {
        // 同尺寸走区域上传(GPU 同步, 无 CPU 像素拷贝):
        // RGB32 内存布局 = BGRA 小端, 直接零拷贝上传
        QOpenGLPixelTransferOptions opt;
        opt.setRowLength(img.bytesPerLine() / 4);   // 处理 stride 可能大于 w*4
        m_tex->setData(0, 0, 0, QOpenGLTexture::CubeMapPositiveX,
                       QOpenGLTexture::BGRA, QOpenGLTexture::UInt8,
                       img.constBits(), &opt);
    }
    doneCurrent();
    update();
}

void GLFrameSurface::clearFrame()
{
    makeCurrent();
    if (m_tex) {
        // 必须彻底删除对象:只 destroy() 会留下"对象在、GPU 资源已释放"的悬空纹理,
        // 新设备重新投屏时 setFrame() 对悬空纹理调 width()/setData() → Qt6OpenGL
        // 访问违规崩溃(2026-08-16 实测:移除一路后重投, 宿主 0xc0000005)。
        // 删对象后 setFrame() 走 if(!m_tex) 分支重建完整纹理。
        m_tex->destroy();
        delete m_tex;
        m_tex = nullptr;
    }
    doneCurrent();
    m_src = m_dst = QRectF();
    update();
}

void GLFrameSurface::paintGL()
{
    auto *gl = QOpenGLContext::currentContext()
                   ? QOpenGLContext::currentContext()->functions() : nullptr;
    if (!gl)
        return;
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    if (!m_tex || m_dst.isEmpty() || !m_prog || !m_prog->isLinked())
        return;
    // GPU 缩放/裁切:纹理坐标取源区域, 顶点(NCD)取目标区域
    // 注意:QImage 上传后图像顶部对应纹理坐标 v=1(OpenGL 原点在左下), 需翻转 v
    const int tw = m_tex->width(), th = m_tex->height();
    const float u0 = float(m_src.left()) / tw;
    const float u1 = float(m_src.right() + 1.0) / tw;
    const float vTop = 1.0f - float(m_src.top()) / th;          // 图像顶部 → v 大
    const float vBot = 1.0f - float(m_src.bottom() + 1.0) / th; // 图像底部 → v 小
    const float w = float(width()), h = float(height());
    const float x0 = float(m_dst.left()) / w * 2.0f - 1.0f;
    const float x1 = float(m_dst.right() + 1.0) / w * 2.0f - 1.0f;
    const float y0 = float(m_dst.bottom()) / h * 2.0f - 1.0f;   // 控件底部 → GL 下方
    const float y1 = float(m_dst.top()) / h * 2.0f - 1.0f;      // 控件顶部 → GL 上方
    // 三角形带:左下 → 右下 → 左上 → 右上; 顶点布局 [x, y, u, v]
    const float verts[4][4] = {
        { x0, y0, u0, vBot },
        { x1, y0, u1, vBot },
        { x0, y1, u0, vTop },
        { x1, y1, u1, vTop },
    };
    const float *vertsf = &verts[0][0];
    m_prog->bind();
    m_tex->bind(0);
    m_prog->setUniformValue("uTex", 0);
    const int posLoc = m_prog->attributeLocation("aPos");
    const int uvLoc  = m_prog->attributeLocation("aUV");
    m_prog->enableAttributeArray(posLoc);
    m_prog->setAttributeArray(posLoc, GL_FLOAT, vertsf, 2, int(sizeof(float) * 4));
    m_prog->enableAttributeArray(uvLoc);
    m_prog->setAttributeArray(uvLoc, GL_FLOAT, vertsf + 2, 2, int(sizeof(float) * 4));
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_prog->disableAttributeArray(posLoc);
    m_prog->disableAttributeArray(uvLoc);
    m_prog->release();
}

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

    // AirPlay 帧率估算(无帧回调):周期抓窗口内容指纹, 变化率 → 动态/静止。
    // 500ms 采样足够判断活动状态, 且避免高频 PrintWindow 抓取增加 CPU 开销。
    m_airStatsTimer.setInterval(500);
    connect(&m_airStatsTimer, &QTimer::timeout, this, [this]() { estimateAirStats(); });

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
        // Miracast 帧模式:GPU 纹理控件显示最新帧(铺满整格)
        m_videoLabel = new GLFrameSurface(this);
        layout->removeWidget(m_videoArea);
        m_videoArea->deleteLater();
        m_videoArea = nullptr;
        layout->addWidget(m_videoLabel, 0, 0);
    }

    // 信息标签:独立顶层小窗(每个播放窗口一个), 叠加在画面左上角。
    // 必须是顶层窗: 嵌入的 D3D11 视频是原生 HWND, 会盖住普通 Qt 子控件。
    // 跟随主窗口靠事件联动(DesktopWindow::moveEvent → refreshInfoBadge 重定位),
    // 主窗口最小化/还原由 SessionView 的 hideEvent/showEvent 同步隐藏/恢复。
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

    // 分辨率/帧率独立标签:排列 名称 · 状态 · 分辨率 · 帧率 · 上下行速率
    m_resLabel = new QLabel(QStringLiteral("--"), m_infoBadge);
    m_resLabel->setStyleSheet("background-color:transparent;");
    m_fpsLabel = new QLabel(QStringLiteral("--"), m_infoBadge);
    m_fpsLabel->setStyleSheet("background-color:transparent;");

    m_devLabel = new QLabel(m_deviceName, m_infoBadge);
    m_devLabel->setStyleSheet("background-color:transparent;");

    auto *badgeL = new QHBoxLayout(m_infoBadge);
    badgeL->setContentsMargins(12, 6, 8, 6);
    badgeL->setSpacing(6);
    badgeL->addWidget(m_devLabel);
    badgeL->addWidget(m_statusDot);
    badgeL->addWidget(m_statusLabel);
    badgeL->addWidget(m_resLabel);
    badgeL->addWidget(m_fpsLabel);
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
    cbs.on_client_info = &SessionView::onClientInfoCallback;
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
    m_airStatsTimer.stop();
    m_airChanged = 0; m_airSamples = 0;
    m_hasAirPrevFp = false;
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

void SessionView::resetToWaiting()
{
    // 清空画面与活跃标记, 回到占位等待(会话保留, 新设备连入后可复用槽位)
    if (m_videoLabel)
        m_videoLabel->clearFrame();
    m_hasFirstFrame = false;
    m_lastThumb = QPixmap();
    // 静音状态重置: 静音是按连接生效的(SETMUTE), 连接断开/移除后旧静音不会
    // 继承到新连接 —— 若不重置, 重新投屏时按钮残留静音图标但实际正常出声。
    m_muted = false;
    if (m_muteBtn)
        m_muteBtn->setText(QStringLiteral("🔊"));
    // AirPlay 断开:停止内容变化检测与速率查询, 避免空抓取空耗
    m_airStatsTimer.stop();
    m_airChanged = 0;
    m_airSamples = 0;
    m_hasAirPrevFp = false;
    m_netTimer.stop();
    emit firstFrameCleared();
    updateInfoBadge();   // 无内容了:悬浮窗随之隐藏
}

void SessionView::detach()
{
    // 移除嵌入的视频窗口容器(视图随后由 DesktopWindow 删除)
    m_thumbTimer.stop();
    m_netTimer.stop();
    m_airStatsTimer.stop();
    m_airChanged = 0;
    m_airSamples = 0;
    m_hasAirPrevFp = false;
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
    m_embeddedHwnd = 0;
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
    QString text;
    if (state == MIRROR_STATE_FAILED)
        text = QStringLiteral("启动失败");
    else if (state == MIRROR_STATE_CLOSED)
        text = QStringLiteral("已关闭");
    else if (state == MIRROR_STATE_STARTING)
        text = QStringLiteral("等待设备投屏连接……");
    else
        text = QStringLiteral("状态 %1").arg(int(state));
    QMetaObject::invokeMethod(self, [self, text, state]() {
        self->setStatus(text);
        if (state == MIRROR_STATE_STARTING) {
            // 帧链路断开(投屏设备退出):清空画面与活跃标记, 回到占位等待。
            // 会话保留(多路 Miracast 服务进程共享), 新设备连入后帧回调重新出画。
            self->resetToWaiting();
        } else if (state == MIRROR_STATE_CLOSED) {
            emit self->sessionClosed(self->m_sessionId);
        }
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
        self->m_framePending = false;   // 渲染完成才解锁, 防止排队帧堆积
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
    // Miracast:帧模式,直接从 GPU 控件读回当前渲染画面
    if (m_videoLabel) {
        QPixmap p = m_videoLabel->grab();
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

// 判断横屏帧左右边缘是否有足够宽的纯黑边(安卓竖屏投屏的特征)。
// 只判断"有无黑边", 不做内容边界采样 → 内容深色不会被误判裁切。
// 要求: 左右各至少 minBlack 列连续近黑(平均亮度<16), 双侧同时成立才视为竖屏内容。
static bool hasSideBars(const QImage &img)
{
    const int w = img.width(), h = img.height();
    if (w < 64 || h < 16 || img.format() != QImage::Format_RGB32)
        return false;
    const int rows[3] = { h * 3 / 8, h / 2, h * 5 / 8 };
    const int minBlack = qMax(16, w / 12);   // 单侧黑边最小宽度(约帧宽 8%)
    const int darkTh = 16;                    // 近黑亮度阈值
    auto colDark = [&](int x) {
        int sum = 0;
        for (int i = 0; i < 3; ++i) {
            const uchar *p = img.constScanLine(rows[i]) + x * 4;  // BGRA
            sum += (p[2] + p[1] + p[0]) / 3;
        }
        return sum / 3 < darkTh;
    };
    int lb = 0;
    for (int x = 0; x < w / 2; ++x) {
        if (colDark(x)) ++lb; else break;
    }
    int rb = 0;
    for (int x = w - 1; x > w / 2; --x) {
        if (colDark(x)) ++rb; else break;
    }
    return lb >= minBlack && rb >= minBlack;
}

void SessionView::renderFrame()
{
    if (!m_sdkSession || !m_videoLabel)
        return;
    mirror_frame_t frame;
    if (mirror_get_frame(m_sdkSession, &frame) != MIRROR_OK) {
        qWarning() << "[view] mirror_get_frame failed";
        return;
    }

    // stride 与宽度对齐(RGB32 每行 4 字节)时零拷贝包装, 避免 8MB 逐行 memcpy;
    // frame.data 在本次调用期间有效, setFrame 内同步上传为纹理, 输出安全。
    QImage img;
    if (frame.stride == frame.width * 4) {
        img = QImage(frame.data, frame.width, frame.height, frame.stride,
                     QImage::Format_RGB32);
    } else {
        img = QImage(frame.width, frame.height, QImage::Format_RGB32);
        const int copyBytes = qMin(frame.stride, img.bytesPerLine());
        for (int y = 0; y < frame.height; ++y) {
            memcpy(img.scanLine(y),
                   frame.data + static_cast<qint64>(y) * frame.stride,
                   copyBytes);
        }
    }
    // ---- 布局计算:缩放/裁切/留边全部由 GPU 完成, CPU 仅算矩形 ----
    // 铺满仅用于 2 分屏:竖屏视频铺满格子, 横屏视频保持原比例(完整可见);
    // 其它分屏(1 屏/3 屏以上)一律保持原比例。只看视频方向, 与格子方向无关。
    // 铺满采用"高度优先":高度 100% 显示(内容不裁切), 宽度超出才裁左右,
    // 不足则居中留深色边 —— 避免竖屏视频上下被裁掉一截。
    const bool framePortrait = frame.height > frame.width;
    // 横屏帧:2 分屏下先判断帧左右边缘是否有足够宽的纯黑边(竖屏内容特征)。
    // 有黑边 → 取帧中央固定 9:16 区域(不做内容边界采样, 深色内容不会被误裁);
    // 无黑边(真横屏内容) → 整帧原样显示, 不做任何裁切。
    QRect barRect(0, 0, frame.width, frame.height);
    if (m_fillMode && !framePortrait && hasSideBars(img)) {
        const int ch = frame.height;
        const int cw = qMin(ch * 9 / 16, frame.width);
        barRect = QRect((frame.width - cw) / 2, 0, cw, ch);
    }
    // 内容为竖屏(帧本身竖屏, 或横屏帧内取出的内容区竖屏)才铺满
    const bool contentPortrait = framePortrait || barRect.width() < barRect.height();
    const bool fill = m_fillMode && contentPortrait;

    const QSizeF target = m_videoLabel->size();
    QRectF src(barRect), dst;
    if (fill) {
        // 高度优先铺满:高度 100%(上下不裁), 宽度超出才裁左右, 不足居中留边
        // 2026-08-16:裁切仅左右各留 30px 黑边, 上下贴满格子边缘。
        const double inset = 30.0;
        const double availW = target.width() - inset * 2;
        const double availH = target.height();
        const double s = availH / src.height();
        const double fitW = src.width() * s;
        if (fitW >= availW) {
            dst = QRectF(inset, 0, availW, availH);
            const double needW = src.width() * availW / fitW;
            const double dx = (src.width() - needW) / 2.0;
            src = QRectF(src.left() + dx, src.top(), needW, src.height());
        } else {
            dst = QRectF(inset + (availW - fitW) / 2.0, 0, fitW, availH);
        }
    } else {
        // 等比留边:完整可见, 居中
        const double s = qMin(target.width() / src.width(),
                              target.height() / src.height());
        const double fw = src.width() * s, fh = src.height() * s;
        dst = QRectF((target.width() - fw) / 2.0,
                     (target.height() - fh) / 2.0, fw, fh);
    }
    m_videoLabel->setFrame(img, src, dst);
    // 分辨率只在速率栏显示(↓↑ Mbps · fps · WxH), 状态文字不重复带
    setStatus(QStringLiteral("接收中"));

    // 首帧:占位会话真正出画, 通知主窗口/列表开始显示
    if (!m_hasFirstFrame) {
        m_hasFirstFrame = true;
        emit firstFrameReceived();
        updateInfoBadge();   // 有内容了:悬浮窗随之显示并贴紧视图
    }

    // 帧率/分辨率统计(1s 窗口):分辨率变化时更新标签, 帧率每秒更新
    ++m_rateFrames;
    if (m_lastW != frame.width || m_lastH != frame.height) {
        m_lastW = frame.width;
        m_lastH = frame.height;
        if (m_resLabel)
            m_resLabel->setText(QStringLiteral("%1×%2").arg(m_lastW).arg(m_lastH));
    }
    if (!m_rateTimer.isValid()) {
        m_rateTimer.start();
    } else if (m_rateTimer.elapsed() >= 1000) {
        m_lastFps = m_rateFrames;
        m_rateFrames = 0;
        m_rateTimer.restart();
        if (m_fpsLabel)
            m_fpsLabel->setText(QStringLiteral("%1fps").arg(m_lastFps));
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
    // 原生统计:GetIfTable2 全接口累计字节差分, 得实时速率。
    // 替代旧的 powershell Get-Counter 方案 —— 每 2.5s 拉一个 powershell
    // 进程会持续出现在进程列表(还带 conhost)并产生可观的启动 CPU 开销。
    double rxBps = -1, txBps = -1;
#ifdef _WIN32
    MIB_IF_TABLE2 *table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table) {
        quint64 rx = 0, tx = 0;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 &r = table->Table[i];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;   // 回环不计, 口径与任务管理器一致
            // InOctets/OutOctets 对部分虚拟接口可能不可用(= -1)
            if (r.InOctets != MAXULONG64)
                rx += r.InOctets;
            if (r.OutOctets != MAXULONG64)
                tx += r.OutOctets;
        }
        FreeMibTable(table);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_netPrevMs > 0 && now > m_netPrevMs) {
            const double secs = double(now - m_netPrevMs) / 1000.0;
            rxBps = double(rx - m_netPrevRx) / secs;   // 无符号差分天然处理 32 位计数回绕
            txBps = double(tx - m_netPrevTx) / secs;
        }
        m_netPrevRx = rx;
        m_netPrevTx = tx;
        m_netPrevMs = now;
    }
#endif
    if (rxBps < 0)
        return;
    if (txBps < 0)
        txBps = 0;
    const double rxMbps = rxBps * 8.0 / 1e6;
    const double txMbps = txBps * 8.0 / 1e6;

    // 只显示上下行速率(分辨率/帧率由独立标签在 renderFrame 更新)
    m_rateLabel->setText(QStringLiteral("↓%1 ↑%2 Mbps")
                             .arg(rxMbps, 0, 'f', 1)
                             .arg(txMbps, 0, 'f', 1));
}

void SessionView::estimateAirStats()
{
    // AirPlay 无帧回调(Miracast 有 on_frame → renderFrame):
    // 分辨率从嵌入的 d3d11videosink 窗口客户区尺寸取, 帧率用内容变化估算。
    if (!m_childWindow || !m_infoBadge)
        return;

    // 1) 分辨率:窗口客户区尺寸即视频尺寸(嵌入的 uxplay 渲染窗口)
    const WId wid = m_childWindow->winId();
    HWND hwnd = reinterpret_cast<HWND>(wid);
    RECT rc{};
    if (hwnd && ::IsWindow(hwnd) && ::GetClientRect(hwnd, &rc) == TRUE
        && rc.right > 0 && rc.bottom > 0) {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w != m_lastW || h != m_lastH) {
            m_lastW = w;
            m_lastH = h;
            if (m_resLabel)
                m_resLabel->setText(QStringLiteral("%1×%2").arg(w).arg(h));
        }
    }

    // 2) 帧率估算:周期抓窗口内容指纹(16x9), 变化采样占比 → 估算活动帧率。
    //    采样频率(250ms)低于真实帧率时只能区分"动态/静止", 无法精确计数,
    //    因此显示区间而非精确值。
    const QImage img = captureThumbnail();
    if (img.isNull())
        return;
    const QImage fp = img.scaled(16, 9, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (!m_hasAirPrevFp) {
        m_airPrevFp = fp;
        m_hasAirPrevFp = true;
        return;   // 首采样仅建立基准
    }
    bool changed = false;
    if (fp != m_airPrevFp) {   // QImage operator== 逐像素比较(144 像素)
        int diff = 0;
        const int total = fp.width() * fp.height();
        for (int y = 0; y < fp.height(); ++y) {
            for (int x = 0; x < fp.width(); ++x) {
                const QRgb a = fp.pixel(x, y);
                const QRgb b = m_airPrevFp.pixel(x, y);
                if (qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                    + qAbs(qBlue(a) - qBlue(b)) > 24)
                    ++diff;
            }
        }
        changed = (double(diff) / total) > 0.01;
    }
    m_airPrevFp = fp;
    if (changed)
        ++m_airChanged;
    ++m_airSamples;

    // 1s 滑动窗口结算:变化采样占比 → 动态/静止(区间)
    if (m_airWin.elapsed() >= 1000) {
        const double pct = m_airSamples ? double(m_airChanged) / m_airSamples : 0.0;
        if (m_fpsLabel) {
            if (pct >= 0.5)
                m_fpsLabel->setText(QStringLiteral("动态"));
            else if (pct > 0.0)
                m_fpsLabel->setText(QStringLiteral("缓慢"));
            else
                m_fpsLabel->setText(QStringLiteral("静止"));
        }
        m_airChanged = 0;
        m_airSamples = 0;
        m_airWin.restart();
    }
}

void SessionView::attachWindow(qulonglong wid)
{
    // 设备断开后 GStreamer/d3d11 会销毁渲染窗口, 重连/新设备可能拿到已失效句柄。
    // 无效句柄直接忽略, 等 on_window 回调(新窗口就绪)再嵌入, 避免 Qt ASSERT 崩溃。
    if (!wid || !::IsWindow(reinterpret_cast<HWND>(wid)))
        return;

    // 同一窗口重复通知(sink 重建/路由抖动)直接忽略, 避免反复销毁重建容器
    if (wid == m_embeddedHwnd)
        return;

    qInfo().nospace() << "[view] attachWindow wid=" << wid
        << " at " << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
        << " session=" << m_sessionId << " prev=" << m_embeddedHwnd;

    // 已嵌入其它窗口(设备切换/路由异常):先同步销毁旧容器再挂新窗口。
    // 不能 deleteLater —— 旧容器与新窗口同时存在时, createWindowContainer
    // 与外部 uxplay 进程对该窗口的操作互相等待, 会卡死 UI 线程。
    if (m_embeddedHwnd && m_childWindow) {
        if (auto *g = qobject_cast<QGridLayout *>(this->layout())) {
            for (int i = g->count() - 1; i >= 0; --i) {
                QLayoutItem *item = g->itemAt(i);
                QWidget *w = item ? item->widget() : nullptr;
                if (w && w != m_infoBadge && w != m_videoArea && w != m_videoLabel) {
                    g->removeWidget(w);
                    w->setParent(nullptr);
                    delete w;
                    break;
                }
            }
        }
        m_childWindow = nullptr;
    }
    m_embeddedHwnd = wid;

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
        // Miracast 帧模式:m_videoLabel 是占位帧控件, swap chain 窗口嵌入后需移除
        if (m_videoLabel) {
            g->removeWidget(m_videoLabel);
            m_videoLabel->deleteLater();
            m_videoLabel = nullptr;
        }
        g->addWidget(container, 0, 0);
        container->show();
    }
    if (m_infoBadge)
        updateInfoBadge();   // 窗口嵌入后:悬浮窗显示并贴紧视图, 且置顶

    setStatus(QStringLiteral("已连接"));
    if (!m_thumbTimer.isActive())
        m_thumbTimer.start();
    // AirPlay 无帧回调:窗口接入后启动内容变化检测(帧率估算)与速率查询。
    // 仅 AirPlay 走此路径(Miracast 由 renderFrame 的帧回调更新统计)。
    if (m_backend == MIRROR_BACKEND_AIRPLAY) {
        m_airWin.start();
        m_airStatsTimer.start();
        m_netTimer.start();
    }
}

void SessionView::updateInfoBadge()
{
    if (!m_infoBadge)
        return;
    // 无投屏内容(尚未出画)或视图不可见:隐藏悬浮窗;有内容才显示并贴紧视图左上角
    const bool hasContent = m_childWindow || m_hasFirstFrame;
    if (!isVisible() || !hasContent) {
        m_infoBadge->hide();
        return;
    }
    // 定位到本视图左上角(全局坐标, 主窗口跨屏移动时由 moveEvent 联动重定位)
    m_infoBadge->move(mapToGlobal(QPoint(12, 12)));
    m_infoBadge->show();
    m_infoBadge->raise();
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
    m_muted = !m_muted;
    if (!applyMute(m_muted)) {
        m_muted = !m_muted;   // 静音控制失败, 回滚
        setStatus(QStringLiteral("静音控制失败"));
        return;
    }
    if (m_muteBtn)
        m_muteBtn->setText(m_muted ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    setStatus(m_muted ? QStringLiteral("已静音") : QStringLiteral("已取消静音"));
}

bool SessionView::applyMute(bool mute)
{
    if (!m_sdkSession)
        return false;
    // Miracast: 组共享同一接收进程, 必须按连接静音(SETMUTE), 不能用进程级 WASAPI
    // (否则整组(含焦点路)都被静音)。
    if (m_backend == MIRROR_BACKEND_MIRACAST) {
        mirror_set_session_mute(m_sdkSession, mute);
        return true;
    }
    // AirPlay/MICE: 每实例独立 uxplay 进程 → WASAPI 按 PID 精确静音
    const uint64_t pid = mirror_get_process_id(m_sdkSession);
    if (!pid) {
        setStatus(QStringLiteral("后端未就绪"));
        return false;
    }
    return mirrorui::setProcessAudioMute(pid, mute);
}

void SessionView::setMuted(bool mute)
{
    if (!m_sdkSession)
        return;
    m_muted = mute;
    applyMute(mute);
    if (m_muteBtn)
        m_muteBtn->setText(mute ? QStringLiteral("🔇") : QStringLiteral("🔊"));
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
    if (!name.isEmpty()) {
        // 真实设备名(如 "Honor 10")替换占位名(Miracast-N):
        // 信息栏标签 + m_deviceName(控制面板列表 sourceItems 取 deviceName())
        m_deviceName = name;
        if (m_devLabel)
            m_devLabel->setText(name);
    }
    if (!name.isEmpty())
        emit clientNameChanged(name);
}

// 设备名就绪(服务端经帧通道上报 MCCTRL1NAME) → 排队主线程更新信息栏
void SessionView::onClientInfoCallback(mirror_session_t *session, const char *client_name,
                                       const char *client_model, void *userdata)
{
    Q_UNUSED(session)
    auto *self = static_cast<SessionView *>(userdata);
    if (!self)
        return;
    const QString name = QString::fromUtf8(client_name ? client_name : "");
    const QString model = QString::fromUtf8(client_model ? client_model : "");
    QMetaObject::invokeMethod(self, [self, name, model]() {
        self->setClientInfo(name, model);
    }, Qt::QueuedConnection);
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
