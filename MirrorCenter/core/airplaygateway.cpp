#include "airplaygateway.h"
#include "mdnsbroadcast.h"
#include "sessionmanager.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QThread>
#include <QDateTime>
#include <QDebug>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mirror {

namespace {
constexpr int kConnectRetryMs = 150;
constexpr int kConnectRetryMax = 60;   // ~9s, 覆盖 GStreamer 首次初始化
}

AirPlayGateway::AirPlayGateway(const Config &cfg, SessionManager *mgr, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_mgr(mgr)
{
    m_nextPort = cfg.instanceBasePort;
}

AirPlayGateway::~AirPlayGateway()
{
    stop();
}

QString AirPlayGateway::findLocalIpv4() const
{
    // 多网卡环境下必须选 iPhone 实际可达的网卡:
    // 1) 优先无线网卡(WLAN/Wi-Fi/Wireless), iOS AirPlay 必然走 Wi-Fi;
    // 2) 否则退回第一个可用 IPv4。
    // 3) 必须排除 Wi-Fi Direct GO 虚拟接口(名字含 "Direct" 或 192.168.137.0/24
    //    网段)。Miracast 接收时系统会创建 GO 接口(192.168.137.1), 它只是投屏
    //    P2P 网段, 广播到该接口 iPhone 永远收不到(实测 AirPlay 搜不到广播的根因)。
    const auto ifaces = QNetworkInterface::allInterfaces();
    QString fallback;

    auto isWifi = [](const QNetworkInterface &i) {
        const QString n = i.name();
        return n.contains(QStringLiteral("WLAN"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Wi-Fi"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Wireless"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("Wireless LAN"), Qt::CaseInsensitive)
            || n.contains(QStringLiteral("无线"), Qt::CaseInsensitive);
    };
    auto isDirectGo = [](const QNetworkInterface &i) {
        if (i.name().contains(QStringLiteral("Direct"), Qt::CaseInsensitive))
            return true;
        const auto entries = i.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            const quint32 ip = e.ip().toIPv4Address();
            // Windows 移动热点 / Wi-Fi Direct GO 默认网段 192.168.137.0/24
            if ((ip & 0xffffff00u) == 0xc0a88900u)   // 192.168.137.x
                return true;
        }
        return false;
    };

    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        if (iface.name().contains(QStringLiteral("vEthernet"), Qt::CaseInsensitive))
            continue;
        if (isDirectGo(iface))
            continue;   // Wi-Fi Direct GO 投屏网段, 不作为 AirPlay 广播接口
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                e.ip().isLoopback() || e.ip().isLinkLocal())
                continue;
            if (isWifi(iface))
                return e.ip().toString();
            if (fallback.isEmpty())
                fallback = e.ip().toString();
        }
    }
    return fallback;
}

quint16 AirPlayGateway::allocBase()
{
    if (!m_freeBases.isEmpty()) {
        quint16 b = *m_freeBases.constBegin();
        m_freeBases.remove(b);
        return b;
    }
    const quint16 b = m_nextPort;
    m_nextPort = quint16(m_nextPort + m_cfg.instanceStep);
    return b;
}

void AirPlayGateway::freeBase(quint16 base)
{
    m_freeBases.insert(base);
}

bool AirPlayGateway::instanceReady(const Instance *inst) const
{
    return inst && inst->session && !inst->pk.isEmpty() && inst->rtspPort != 0;
}

AirPlayGateway::Instance *AirPlayGateway::createInstance(const QString &clientIp)
{
    if (m_stopping)
        return nullptr;
    if (m_instances.size() >= m_cfg.maxInstances) {
        emit logMessage(QStringLiteral("[gateway] 实例数已达上限 %1").arg(m_cfg.maxInstances));
        return nullptr;
    }

    const quint16 base = allocBase();
    QStringList args;
    args << QStringLiteral("-silent")
         << QStringLiteral("-p") << QString::number(base)
         << QStringLiteral("-m") << m_cfg.mac
         << QStringLiteral("-key") << m_cfg.keyfile
         << QStringLiteral("-n") << m_cfg.deviceName;
    args << m_cfg.extraArgs;

    MirrorSession *s = m_mgr->createSession(BackendType::AirPlay,
                                            m_cfg.deviceName,
                                            m_cfg.backendExe, args);
    if (!s) {
        freeBase(base);
        emit logMessage(QStringLiteral("[gateway] 创建实例失败(base=%1)").arg(base));
        return nullptr;
    }

    auto *inst = new Instance;
    inst->basePort = base;
    inst->session = s;
    inst->rtspPort = quint16(base + 1);   // -p base 时 RTSP 主端口为 tcp[1]=base+1

    connect(s, &MirrorSession::logMessage, this,
            [this, inst](const QString &, const QString &line) { onSessionLog(inst, line); });
    connect(s, &MirrorSession::windowReady, this,
            [this, inst](const QString &, qulonglong h) { onInstanceWindow(inst, h); });
    connect(s, &MirrorSession::stateChanged, this,
            [this, inst](const QString &, SessionState st) {
        if ((st == SessionState::Closed || st == SessionState::Failed) && !m_stopping) {
            // 实例进程意外退出 → 立即回收
            if (m_instances.contains(inst)) {
                destroyInstance(inst);
                maybeStartMdns();
            }
        }
    });

    inst->recycleTimer.setSingleShot(true);
    connect(&inst->recycleTimer, &QTimer::timeout, this, [this, inst]() {
        if (m_instances.contains(inst) && !inst->clientSock)
            destroyInstance(inst);
    });

    m_instances.append(inst);
    s->start();

    emit logMessage(QStringLiteral("[gateway] 启动实例 base=%1 端口=%2 %3")
                        .arg(base).arg(inst->rtspPort)
                        .arg(clientIp.isEmpty() ? QStringLiteral("(就绪实例)") : QStringLiteral("→ %1").arg(clientIp)));
    return inst;
}

void AirPlayGateway::destroyInstance(Instance *inst)
{
    if (!inst || !m_instances.contains(inst))
        return;
    m_instances.removeAll(inst);

    if (!inst->clientIp.isEmpty() && m_clientMap.value(inst->clientIp) == inst)
        m_clientMap.remove(inst->clientIp);

    stopNtpTunnel(inst);
    if (inst->clientSock) {
        inst->clientSock->disconnect(this);
        inst->clientSock->abort();
        inst->clientSock->deleteLater();
        inst->clientSock = nullptr;
    }
    if (inst->instSock) {
        inst->instSock->disconnect(this);
        inst->instSock->abort();
        inst->instSock->deleteLater();
        inst->instSock = nullptr;
    }
    inst->recycleTimer.stop();

    const QString ip = inst->clientIp;
    if (inst->session) {
        inst->session->disconnect(this);
        inst->session->stop();
        inst->session->deleteLater();
    }

    freeBase(inst->basePort);
    emit logMessage(QStringLiteral("[gateway] 实例已回收 base=%1%2")
                        .arg(inst->basePort)
                        .arg(ip.isEmpty() ? QString() : QStringLiteral(" (设备 %1)").arg(ip)));

    delete inst;

    // 保持至少 1 个空闲实例(预置)
    if (!m_stopping) {
        int idle = 0;
        for (Instance *i : std::as_const(m_instances))
            if (!i->clientSock)
                ++idle;
        if (idle == 0 && m_instances.size() < m_cfg.maxInstances)
            createInstance(QString());
    }
}

void AirPlayGateway::onSessionLog(Instance *inst, const QString &line)
{
    if (!inst)
        return;

    static const QRegularExpression pkRe(QStringLiteral("MIRROR_PK=([0-9a-fA-F]{64})"));
    static const QRegularExpression portRe(QStringLiteral("MIRROR_RTSP_PORT=(\\d+)"));
    static const QRegularExpression timingRe(QStringLiteral("timing_rport\\s*=\\s*(\\d+)"));
    static const QRegularExpression nameRe(QStringLiteral("^MIRROR_CLIENT_NAME=(.*)$"));
    static const QRegularExpression modelRe(QStringLiteral("^MIRROR_CLIENT_MODEL=(.*)$"));
    static const QRegularExpression decoderRe(QStringLiteral("^MIRROR_DECODER=(\\S+)$"));

    const auto pkMatch = pkRe.match(line);
    if (pkMatch.hasMatch() && inst->pk.isEmpty()) {
        inst->pk = pkMatch.captured(1);
        emit logMessage(QStringLiteral("[gateway] 实例 base=%1 报告 pk=%2…")
                            .arg(inst->basePort).arg(inst->pk.left(8)));
        maybeStartMdns();
    }

    const auto portMatch = portRe.match(line);
    if (portMatch.hasMatch()) {
        inst->rtspPort = quint16(portMatch.captured(1).toUShort());
        emit logMessage(QStringLiteral("[gateway] 实例 base=%1 RTSP 端口确认 %2")
                            .arg(inst->basePort).arg(inst->rtspPort));
    }

    const auto timingMatch = timingRe.match(line);
    if (timingMatch.hasMatch()) {
        const quint16 timingPort = quint16(timingMatch.captured(1).toUShort());
        if (inst->clientSock && !inst->ntpSock)
            beginNtpTunnel(inst, timingPort);
    }

    // 实际视频解码器(软解 avdec_h264 / 硬解 d3d11h264dec 等) → 软/硬解能力
    const auto decoderMatch = decoderRe.match(line);
    if (decoderMatch.hasMatch() && inst->decoder.isEmpty()) {
        inst->decoder = decoderMatch.captured(1);
        emit decoderChanged(inst->session, inst->decoder);
        emit logMessage(QStringLiteral("[gateway] 实例 base=%1 解码器=%2")
                            .arg(inst->basePort).arg(inst->decoder));
    }

    // 来源手机名称/型号(仅在设备已绑定实例时上报,避免空闲实例误报)
    bool infoChanged = false;
    const auto nameMatch = nameRe.match(line);
    if (nameMatch.hasMatch() && inst->clientSock) {
        inst->clientName = nameMatch.captured(1).trimmed();
        infoChanged = true;
    }
    const auto modelMatch = modelRe.match(line);
    if (modelMatch.hasMatch() && inst->clientSock) {
        inst->clientModel = modelMatch.captured(1).trimmed();
        infoChanged = true;
    }
    if (infoChanged && inst->clientSock) {
        emit clientInfoChanged(inst->session, inst->clientName, inst->clientModel);
        emit logMessage(QStringLiteral("[gateway] 设备 %1: %2 (%3)")
                            .arg(inst->clientIp, inst->clientName, inst->clientModel));
    }
}

void AirPlayGateway::onInstanceWindow(Instance *inst, qulonglong handle)
{
#ifdef _WIN32
    if (!handle)
        return;
    emit logMessage(QStringLiteral("[gateway] 实例 base=%1 窗口句柄=%2 (此刻 %3)")
                        .arg(inst ? QString::number(inst->basePort) : QStringLiteral("?"))
                        .arg(handle)
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    // 空闲实例的 d3d11 窗口先隐藏,设备连入后由 UI 嵌入显示
    if (inst && !inst->clientSock)
        ::ShowWindow(reinterpret_cast<HWND>(handle), SW_HIDE);
#else
    Q_UNUSED(inst)
    Q_UNUSED(handle)
#endif
}

void AirPlayGateway::maybeStartMdns()
{
    if (m_mdnsStarted)
        return;
    if (m_sharedPk.isEmpty()) {
        for (Instance *i : std::as_const(m_instances)) {
            if (!i->pk.isEmpty()) {
                m_sharedPk = i->pk;
                break;
            }
        }
    }
    if (m_sharedPk.isEmpty())
        return;

    const QString ipv4 = findLocalIpv4();
    if (ipv4.isEmpty()) {
        emit logMessage(QStringLiteral("[gateway] 未找到可用局域网 IPv4, 放弃广播"));
        return;
    }
    const QHostAddress addr(ipv4);

    const QString hostName = QStringLiteral("mirrorcenter.local");

    QString macUpper = m_cfg.mac;
    macUpper.remove(QLatin1Char(':'));
    macUpper = macUpper.toUpper();

    QList<MdnsBroadcaster::Service> services;

    MdnsBroadcaster::Service ap;
    ap.name = m_cfg.deviceName;
    ap.type = QStringLiteral("_airplay._tcp");
    ap.port = m_cfg.gatewayPort;
    ap.txt = {
        {QStringLiteral("deviceid"), m_cfg.mac.toLower()},
        {QStringLiteral("features"), QStringLiteral("0x527FFEE6,0x0")},
        {QStringLiteral("pw"), QStringLiteral("false")},
        {QStringLiteral("flags"), QStringLiteral("0x4")},
        {QStringLiteral("model"), QStringLiteral("AppleTV3,2")},
        {QStringLiteral("pk"), m_sharedPk},
        {QStringLiteral("pi"), QStringLiteral("2e388006-13ba-4041-9a67-25dd4a43d536")},
        {QStringLiteral("srcvers"), QStringLiteral("220.68")},
        {QStringLiteral("vv"), QStringLiteral("2")},
    };
    services.append(ap);

    MdnsBroadcaster::Service raop;
    raop.name = QStringLiteral("%1@%2").arg(macUpper, m_cfg.deviceName);
    raop.type = QStringLiteral("_raop._tcp");
    raop.port = m_cfg.gatewayPort;
    raop.txt = {
        {QStringLiteral("ch"), QStringLiteral("2")},
        {QStringLiteral("cn"), QStringLiteral("0,1,2,3")},
        {QStringLiteral("da"), QStringLiteral("true")},
        {QStringLiteral("et"), QStringLiteral("0,3,5")},
        {QStringLiteral("vv"), QStringLiteral("2")},
        {QStringLiteral("ft"), QStringLiteral("0x5A7FFEE6,0x0")},
        {QStringLiteral("am"), QStringLiteral("AppleTV3,2")},
        {QStringLiteral("md"), QStringLiteral("0,1,2")},
        {QStringLiteral("rhd"), QStringLiteral("5.6.0.0")},
        {QStringLiteral("pw"), QStringLiteral("false")},
        {QStringLiteral("sr"), QStringLiteral("44100")},
        {QStringLiteral("ss"), QStringLiteral("16")},
        {QStringLiteral("sv"), QStringLiteral("false")},
        {QStringLiteral("tp"), QStringLiteral("UDP")},
        {QStringLiteral("txtvers"), QStringLiteral("1")},
        {QStringLiteral("sf"), QStringLiteral("0x4")},
        {QStringLiteral("vs"), QStringLiteral("220.68")},
        {QStringLiteral("vn"), QStringLiteral("65537")},
        {QStringLiteral("pk"), m_sharedPk},
    };
    services.append(raop);

    if (!m_mdns) {
        m_mdns = new MdnsBroadcaster(this);
        connect(m_mdns, &MdnsBroadcaster::destroyed, this, [this]() { m_mdns = nullptr; });
    }
    if (!m_mdns->start(hostName, addr, services)) {
        emit logMessage(QStringLiteral("[gateway] mDNS 广播启动失败"));
        return;
    }

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *sock = m_server->nextPendingConnection())
                routeConnection(sock);
        });
    }
    if (!m_server->isListening() && !m_server->listen(QHostAddress::AnyIPv4, m_cfg.gatewayPort)) {
        emit logMessage(QStringLiteral("[gateway] 监听 %1 失败: %2")
                            .arg(m_cfg.gatewayPort).arg(m_server->errorString()));
        return;
    }

    m_mdnsStarted = true;
    emit logMessage(QStringLiteral("[gateway] 广播 %1 → TCP %2 (IP %3)")
                        .arg(m_cfg.deviceName).arg(m_cfg.gatewayPort).arg(ipv4));
    emit gatewayReady(m_cfg.deviceName, m_cfg.gatewayPort);
}

bool AirPlayGateway::start()
{
    if (m_running)
        return true;
    m_running = true;
    m_stopping = false;

    if (m_cfg.keyfile.isEmpty()) {
        emit logMessage(QStringLiteral("[gateway] 缺少 keyfile 配置"));
        m_running = false;
        return false;
    }

    if (!createInstance(QString())) {
        emit logMessage(QStringLiteral("[gateway] 就绪实例启动失败"));
        m_running = false;
        return false;
    }
    return true;
}

void AirPlayGateway::stop()
{
    if (m_stopping)
        return;
    m_stopping = true;
    m_running = false;

    emit logMessage(QStringLiteral("[gateway] stop() 被调用(关闭 mDNS/监听/实例)"));
    QThread *t = QThread::currentThread();
    emit logMessage(QStringLiteral("[gateway] stop() 调用线程=%1")
                        .arg(t ? t->objectName() : QStringLiteral("<unknown>")));

    if (m_server) {
        m_server->disconnect(this);
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_mdns) {
        m_mdns->disconnect(this);
        m_mdns->deleteLater();
        m_mdns = nullptr;
    }

    for (Instance *inst : std::as_const(m_instances)) {
        stopNtpTunnel(inst);
        if (inst->clientSock) {
            inst->clientSock->disconnect(this);
            inst->clientSock->abort();
        }
        if (inst->instSock) {
            inst->instSock->disconnect(this);
            inst->instSock->abort();
        }
        inst->recycleTimer.stop();
        if (inst->session) {
            inst->session->disconnect(this);
            inst->session->stop();
            inst->session->deleteLater();
        }
        delete inst;
    }
    m_instances.clear();
    m_clientMap.clear();
    m_freeBases.clear();
    m_sharedPk.clear();
    m_mdnsStarted = false;
    m_stopping = false;
}

void AirPlayGateway::routeConnection(QTcpSocket *sock)
{
    if (!sock)
        return;
    // 双栈模式下 peerAddress 可能返回 IPv4-mapped(::ffff:1.2.3.4),
    // 必须归一化为纯 IPv4, 否则与 UDP 隧道收到的 IPv4 源地址无法匹配
    QHostAddress peer = sock->peerAddress();
    if (peer.isLoopback()) {
        // 本机探测/健康检查连接(如 127.0.0.1:7100), 不分配实例
        sock->disconnectFromHost();
        sock->deleteLater();
        return;
    }
    QString ip = peer.toString();
    if (peer.protocol() == QAbstractSocket::IPv6Protocol && ip.startsWith(QLatin1String("::ffff:")))
        ip = QHostAddress(peer.toIPv4Address()).toString();
    // AirPlay 视频链路(mirror data/控制通道)仅支持 IPv4:若设备经 IPv6 连入,
    // 视频数据流无法送达 uxplay 的 IPv4-only 监听端口, 表现为"无图像"。
    // 直接拒绝(abort→RST), 让 iOS 的 Happy Eyeballs 回退到 IPv4 重连。
    if (peer.protocol() == QAbstractSocket::IPv6Protocol) {
        emit logMessage(QStringLiteral("[gateway] 拒绝 IPv6 连接 %1 (AirPlay 视频链路仅支持 IPv4)").arg(ip));
        sock->abort();
        sock->deleteLater();
        return;
    }

    Instance *inst = nullptr;
    if (m_clientMap.contains(ip)) {
        Instance *c = m_clientMap.value(ip);
        if (c->clientSock) {
            // 同一设备在已有活跃连接时再次连接(异常) → 拒绝, 由 iOS 自行处理
            emit logMessage(QStringLiteral("[gateway] 设备 %1 重复连接, 拒绝").arg(ip));
            sock->disconnectFromHost();
            sock->deleteLater();
            return;
        }
        // 30s 回收期内同设备重连 → 复用原实例
        inst = c;
        cancelRecycle(inst);
        emit logMessage(QStringLiteral("[gateway] 设备 %1 重连, 复用实例 base=%2")
                            .arg(ip).arg(inst->basePort));
    }

    if (!inst) {
        for (Instance *i : std::as_const(m_instances)) {
            if (!i->clientSock) { inst = i; break; }
        }
    }
    if (!inst)
        inst = createInstance(ip);
    if (!inst) {
        sock->disconnectFromHost();
        sock->deleteLater();
        return;
    }

    cancelRecycle(inst);
    inst->clientIp = ip;
    inst->clientSock = sock;
    inst->connectRetries = 0;
    m_clientMap.insert(ip, inst);

    connect(sock, &QTcpSocket::readyRead, this, [this, inst]() { flushClientToInstance(inst); });
    connect(sock, &QTcpSocket::disconnected, this, [this, inst]() { onClientSocketGone(inst); });

    connectToInstance(inst);
#ifdef _WIN32
    // 设备连入:显示实例窗口(空闲时已被隐藏), 供 UI 嵌入
    if (inst->session) {
        const qulonglong h = inst->session->windowHandle();
        if (h)
            ::ShowWindow(reinterpret_cast<HWND>(h), SW_SHOW);
    }
#endif
    emit clientConnected(ip, inst->session);
}

void AirPlayGateway::connectToInstance(Instance *inst)
{
    if (!inst->clientSock || inst->instSock)
        return;

    auto *s = new QTcpSocket(this);
    inst->instSock = s;

    connect(s, &QTcpSocket::connected, this, [this, inst]() {
        if (inst->instSock)
            inst->instSock->setProperty("_connected", true);
        flushClientToInstance(inst);
    });
    connect(s, &QTcpSocket::readyRead, this, [this, inst]() { flushInstanceToClient(inst); });
    connect(s, &QTcpSocket::errorOccurred, this,
            [this, inst](QAbstractSocket::SocketError) { retryConnectToInstance(inst); });
    connect(s, &QTcpSocket::disconnected, this, [this, inst]() { onInstanceSocketGone(inst); });

    s->connectToHost(QHostAddress::LocalHost, inst->rtspPort);
}

void AirPlayGateway::retryConnectToInstance(Instance *inst)
{
    if (m_stopping || !inst)
        return;

    if (inst->instSock) {
        inst->instSock->disconnect(this);
        inst->instSock->deleteLater();
        inst->instSock = nullptr;
    }

    if (!inst->clientSock) {
        // 客户端已断开, 不再重试
        teardownLink(inst);
        return;
    }
    if (++inst->connectRetries > kConnectRetryMax) {
        emit logMessage(QStringLiteral("[gateway] 连接实例 base=%1 超时, 断开设备")
                            .arg(inst->basePort));
        inst->clientSock->abort();
        return;
    }

    QTimer::singleShot(kConnectRetryMs, this, [this, inst]() {
        if (inst->clientSock && !inst->instSock)
            connectToInstance(inst);
    });
}

void AirPlayGateway::flushClientToInstance(Instance *inst)
{
    if (!inst || !inst->clientSock || !inst->instSock)
        return;
    if (inst->instSock->state() != QAbstractSocket::ConnectedState)
        return;
    const QByteArray data = inst->clientSock->readAll();
    if (!data.isEmpty())
        inst->instSock->write(data);
}

void AirPlayGateway::flushInstanceToClient(Instance *inst)
{
    if (!inst || !inst->instSock || !inst->clientSock)
        return;
    const QByteArray data = inst->instSock->readAll();
    if (!data.isEmpty())
        inst->clientSock->write(data);
}

void AirPlayGateway::onClientSocketGone(Instance *inst)
{
    if (!inst)
        return;
    if (inst->clientSock) {
        inst->clientSock->disconnect(this);
        inst->clientSock->deleteLater();
        inst->clientSock = nullptr;
    }
    teardownLink(inst);
}

void AirPlayGateway::onInstanceSocketGone(Instance *inst)
{
    if (!inst)
        return;
    if (inst->instSock) {
        inst->instSock->disconnect(this);
        inst->instSock->deleteLater();
        inst->instSock = nullptr;
    }
    teardownLink(inst);
}

void AirPlayGateway::teardownLink(Instance *inst)
{
    if (!inst)
        return;
    const QString ip = inst->clientIp;

    stopNtpTunnel(inst);
    if (inst->clientSock) {
        inst->clientSock->disconnect(this);
        inst->clientSock->abort();
        inst->clientSock->deleteLater();
        inst->clientSock = nullptr;
    }
    if (inst->instSock) {
        inst->instSock->disconnect(this);
        inst->instSock->deleteLater();
        inst->instSock = nullptr;
    }
    inst->connectRetries = 0;

    if (!ip.isEmpty() && m_clientMap.value(ip) == inst)
        m_clientMap.remove(ip);
    inst->clientIp.clear();
    inst->clientName.clear();
    inst->clientModel.clear();

    if (!ip.isEmpty())
        emit clientDisconnected(ip, inst->session);

    if (m_stopping)
        return;
    // 空闲实例窗口隐藏
    if (inst->session) {
#ifdef _WIN32
        const qulonglong h = inst->session->windowHandle();
        if (h)
            ::ShowWindow(reinterpret_cast<HWND>(h), SW_HIDE);
#endif
    }
    startRecycle(inst);
}

void AirPlayGateway::startRecycle(Instance *inst)
{
    if (!inst || m_stopping)
        return;
    inst->recycleTimer.start(m_cfg.recycleMs);
    emit logMessage(QStringLiteral("[gateway] 实例 base=%1 空闲, %2ms 后回收")
                        .arg(inst->basePort).arg(m_cfg.recycleMs));
}

void AirPlayGateway::cancelRecycle(Instance *inst)
{
    if (inst)
        inst->recycleTimer.stop();
}

void AirPlayGateway::beginNtpTunnel(Instance *inst, quint16 timingPort)
{
    if (!inst || !inst->clientSock || inst->ntpSock)
        return;

    auto *udp = new QUdpSocket(this);
    // 绑定 0.0.0.0:timingPort:实例 NTP 请求发往 127.0.0.1:timingPort,
    // iPhone 响应发往 主机IP:timingPort, 两者都落在此 socket 上。
    if (!udp->bind(QHostAddress::AnyIPv4, timingPort,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit logMessage(QStringLiteral("[gateway] NTP 隧道绑定端口 %1 失败: %2")
                            .arg(timingPort).arg(udp->errorString()));
        delete udp;
        return;
    }

    inst->ntpSock = udp;
    inst->ntpLocalPort = timingPort;

    connect(udp, &QUdpSocket::readyRead, this, [this, inst]() {
        QUdpSocket *udp = inst->ntpSock;
        if (!udp)
            return;
        while (udp->hasPendingDatagrams()) {
            QByteArray dg;
            dg.resize(int(udp->pendingDatagramSize()));
            QHostAddress from;
            quint16 fromPort = 0;
            udp->readDatagram(dg.data(), dg.size(), &from, &fromPort);
            if (dg.isEmpty())
                continue;
            if (!inst->clientSock)
                continue;
            if (from.isLoopback()) {
                // 实例 → iPhone(改写源 IP/端口为网关 socket)
                udp->writeDatagram(dg, QHostAddress(inst->clientIp), inst->ntpLocalPort);
            } else if (from.toString() == inst->clientIp) {
                // iPhone → 实例(转发到实例 timing_lport = base)
                udp->writeDatagram(dg, QHostAddress::LocalHost, inst->basePort);
            }
        }
    });

    emit logMessage(QStringLiteral("[gateway] NTP 隧道建立: 127.0.0.1:%1 ↔ %2:%1")
                        .arg(timingPort).arg(inst->clientIp));
}

void AirPlayGateway::stopNtpTunnel(Instance *inst)
{
    if (!inst)
        return;
    if (inst->ntpSock) {
        inst->ntpSock->disconnect(this);
        inst->ntpSock->close();
        inst->ntpSock->deleteLater();
        inst->ntpSock = nullptr;
        inst->ntpLocalPort = 0;
    }
}

} // namespace mirror
