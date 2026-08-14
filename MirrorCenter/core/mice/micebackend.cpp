#include "micebackend.h"

#include "mdnsbroadcast.h"
#include "micertsp.h"
#include "micertp.h"
#include "micedecoder.h"

#include <functional>
#include <QUuid>
#include <QNetworkInterface>
#include <QSysInfo>
#include <QDebug>

namespace mirror {

/**
 * 单路 MS-MICE 会话: RTP 接收 + RTSP 协商 + H.264 解码。
 * 生命周期由 MiceBackend 管理; RTSP 失败/关闭时自删(deleteLater)。
 */
class MiceSession : public QObject
{
public:
    using FrameCb = std::function<void(const QImage &)>;

    MiceSession(const MiceSourceInfo &src, quint16 rtpBase, FrameCb cb, QObject *parent)
        : QObject(parent)
        , m_info(src)
        , m_frameCb(std::move(cb))
        , m_rtpBase(rtpBase)
    {
    }

    ~MiceSession() override
    {
        if (m_decoder)
            m_decoder->stop();
    }

    bool start()
    {
        m_rtp = new MiceRtpReceiver(this);
        m_rtpPort = m_rtp->start(m_rtpBase);
        if (!m_rtpPort) {
            qWarning() << "[mice-session] no RTP port available for"
                       << m_info.friendlyName;
            return false;
        }

        m_decoder = new MiceDecoder(this);
        m_decoder->start();
        connect(m_rtp, &MiceRtpReceiver::annexBFrame, this,
                [this](const QByteArray &frame) { m_decoder->pushFrame(frame); });
        connect(m_decoder, &MiceDecoder::frameReady, this,
                [this](const QImage &img) { m_frameCb(img); });
        connect(m_decoder, &MiceDecoder::error, this,
                [this](const QString &why) { qWarning() << "[mice-session] decode:" << why; });

        m_rtsp = new MiceRtsp(this);
        connect(m_rtsp, &MiceRtsp::negotiated, this, [this] {
            qInfo() << "[mice-session] negotiated with" << m_info.friendlyName
                    << "rtp port" << m_rtpPort;
        });
        connect(m_rtsp, &MiceRtsp::failed, this, [this](const QString &why) {
            qWarning() << "[mice-session] rtsp failed:" << why;
            m_rtsp->teardown();
            deleteLater();
        });
        connect(m_rtsp, &MiceRtsp::remoteClosed, this, [this] {
            qInfo() << "[mice-session] source closed rtsp";
            deleteLater();
        });
        m_rtsp->startNegotiation(m_info.sourceIp, m_info.rtspPort, m_rtpPort);
        return true;
    }

private:
    MiceSourceInfo m_info;
    FrameCb m_frameCb;
    quint16 m_rtpBase = 0;
    quint16 m_rtpPort = 0;
    MiceRtpReceiver *m_rtp = nullptr;
    MiceRtsp *m_rtsp = nullptr;
    MiceDecoder *m_decoder = nullptr;
};

namespace {

QString guidWithoutBraces()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
}

} // namespace

MiceBackend::MiceBackend(const Config &cfg, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_containerId(cfg.containerId.isEmpty() ? guidWithoutBraces() : cfg.containerId)
{
}

MiceBackend::~MiceBackend()
{
    stop();
}

bool MiceBackend::start()
{
    stop();

    const QString ip = findLocalIpv4();
    if (ip.isEmpty()) {
        qWarning() << "[mice] no usable IPv4 address";
        return false;
    }
    const QHostAddress ipv4(ip);

    // mDNS 发布 _display._tcp(container_id 标识 Sink)
    m_mdns = new MdnsBroadcaster(this);
    MdnsBroadcaster::Service svc;
    svc.name = m_cfg.deviceName;
    svc.type = QStringLiteral("_display._tcp");
    svc.port = m_cfg.micePort;
    svc.txt = { { QStringLiteral("container_id"), m_containerId } };
    const QString hostName =
        QString::fromLatin1(m_cfg.deviceName.toLatin1().toLower().replace(' ', "-")) + ".local";
    // Windows 发送端解析 MS-MICE Sink 用的是 WFD Host Name 属性(0x2002),
    // 系统按电脑名自动广播且无法修改, 所以 mDNS 必须同时响应电脑名。
    // 注意电脑名可能含非 ASCII 字符, 需剔除以保证 DNS 名称合法。
    QString pcName = QSysInfo::machineHostName();
    QString ascii;
    ascii.reserve(pcName.size());
    for (const QChar &c : pcName) {
        if (c.isUpper())
            ascii.append(c.toLower());
        else if (c.isLower() || c.isDigit() || c == QLatin1Char('-'))
            ascii.append(c);
    }
    if (!ascii.isEmpty())
        m_mdns->setHostAliases({ ascii + QStringLiteral(".local") });
    const QHostAddress ipv6 = findLocalIpv6();
    if (!m_mdns->start(hostName, ipv4, { svc }, ipv6)) {
        qWarning() << "[mice] mDNS start failed";
    } else {
        qInfo() << "[mice] mDNS advertising _display._tcp container_id=" << m_containerId;
    }

    // TCP 7250 控制通道
    m_server = new MiceServer(this);
    connect(m_server, &MiceServer::sourceReady, this, &MiceBackend::onSourceReady);
    connect(m_server, &MiceServer::sourceDisconnected, this, &MiceBackend::onSourceDisconnected);
    if (!m_server->start(m_cfg.micePort)) {
        qWarning() << "[mice] failed to listen on TCP" << m_cfg.micePort;
        stop();
        return false;
    }
    qInfo() << "[mice] listening on TCP" << m_cfg.micePort;

    m_running = true;
    emit logMessage(QStringLiteral("MS-MICE receiver ready (port %1)").arg(m_cfg.micePort));
    return true;
}

void MiceBackend::stop()
{
    const auto sessions = m_sessions.values();
    m_sessions.clear();
    qDeleteAll(sessions);

    if (m_server) {
        m_server->disconnect(this);
        delete m_server;
        m_server = nullptr;
    }
    if (m_mdns) {
        m_mdns->disconnect(this);
        delete m_mdns;
        m_mdns = nullptr;
    }
    m_running = false;
}

quint16 MiceBackend::micePort() const
{
    return m_server ? m_server->port() : m_cfg.micePort;
}

QString MiceBackend::containerId() const
{
    return m_containerId;
}

void MiceBackend::onSourceReady(const MiceSourceInfo &info)
{
    QByteArray key = info.sourceId;
    if (key.isEmpty()) {
        key = info.sourceIp.toString().toLatin1() + ":" + QByteArray::number(info.rtspPort);
    }
    if (m_sessions.contains(key)) {
        qWarning() << "[mice] source already connected:" << info.friendlyName;
        return;
    }
    if (m_sessions.size() >= m_cfg.maxSessions) {
        qWarning() << "[mice] max sessions reached, rejecting" << info.friendlyName;
        return;
    }

    MiceSession *session = new MiceSession(info, m_cfg.rtpBasePort,
        [this, key](const QImage &img) { emit frameReady(key, img); }, this);
    connect(session, &QObject::destroyed, this,
            [this, key] {
                m_sessions.remove(key);
                emit sourceDisconnected(key);
            });
    if (!session->start()) {
        delete session;
        return;
    }
    m_sessions.insert(key, session);
    qInfo() << "[mice] source connected:" << info.friendlyName
            << "ip" << info.sourceIp.toString() << "rtsp" << info.rtspPort;
    emit sourceConnected(info.friendlyName, key);
}

void MiceBackend::onSourceDisconnected(const QByteArray &sourceId)
{
    MiceSession *session = m_sessions.take(sourceId);
    if (session) {
        qInfo() << "[mice] source disconnected:" << QString::fromLatin1(sourceId);
        delete session;
        emit sourceDisconnected(sourceId);
    }
}

QString MiceBackend::findLocalIpv4() const
{
    const auto ifaces = QNetworkInterface::allInterfaces();
    QString fallback;
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        if (iface.name().contains(QStringLiteral("vEthernet"), Qt::CaseInsensitive))
            continue;
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                e.ip().isLoopback() || e.ip().isLinkLocal())
                continue;
            if (fallback.isEmpty())
                fallback = e.ip().toString();
        }
    }
    return fallback;
}

QHostAddress MiceBackend::findLocalIpv6() const
{
    // 取与 IPv4 同一物理接口的 IPv6(含 link-local)。
    // AAAA 记录用于 Windows 双栈解析 hostname; link-local 在同一 L2 内可达。
    const QHostAddress v4(findLocalIpv4());
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        if (iface.name().contains(QStringLiteral("vEthernet"), Qt::CaseInsensitive))
            continue;
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &e : entries) {
            if (e.ip().protocol() != QAbstractSocket::IPv6Protocol ||
                e.ip().isLoopback())
                continue;
            if (!v4.isNull()) {
                // 优先同一接口上的 IPv6
                bool sameIface = false;
                for (const QNetworkAddressEntry &e2 : iface.addressEntries()) {
                    if (e2.ip() == v4) {
                        sameIface = true;
                        break;
                    }
                }
                if (!sameIface)
                    continue;
            }
            return e.ip();
        }
    }
    return QHostAddress();
}

} // namespace mirror
