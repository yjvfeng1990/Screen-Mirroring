#include "mdnsbroadcast.h"

#include <QUdpSocket>
#include <QTimer>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QDebug>

namespace mirror {

namespace {
constexpr quint16 kMdnsPort = 5353;
constexpr quint32 kTtlService = 4500;
constexpr quint32 kTtlHost = 120;

constexpr quint16 kTypeA = 1;
constexpr quint16 kTypePTR = 12;
constexpr quint16 kTypeTXT = 16;
constexpr quint16 kTypeSRV = 33;
constexpr quint16 kTypeANY = 255;
constexpr char kServicesDnsSdName[] = "_services._dns-sd._udp.local";

void putU16(QByteArray &p, quint16 v)
{
    p.append(char((v >> 8) & 0xff));
    p.append(char(v & 0xff));
}

void putU32(QByteArray &p, quint32 v)
{
    p.append(char((v >> 24) & 0xff));
    p.append(char((v >> 16) & 0xff));
    p.append(char((v >> 8) & 0xff));
    p.append(char(v & 0xff));
}

/* 解析 DNS 名称(支持压缩指针)。返回解析后名称与消费长度;失败返回 false。 */
bool decodeName(const QByteArray &pkt, int &pos, QString &out)
{
    QString name;
    int jumps = 0;
    int p = pos;
    bool jumped = false;

    while (p < pkt.size()) {
        const quint8 len = quint8(pkt[p]);
        if (len == 0) {
            ++p;
            if (!jumped)
                pos = p;
            out = name;
            return true;
        }
        if ((len & 0xc0) == 0xc0) {
            if (p + 1 >= pkt.size() || ++jumps > 16)
                return false;
            const int ptr = ((len & 0x3f) << 8) | quint8(pkt[p + 1]);
            if (ptr >= pkt.size())
                return false;
            if (!jumped)
                pos = p + 2;
            p = ptr;
            jumped = true;
            continue;
        }
        if ((len & 0xc0) || p + 1 + len > pkt.size())
            return false;
        if (!name.isEmpty())
            name += QLatin1Char('.');
        name += QString::fromLatin1(pkt.constData() + p + 1, len);
        p += 1 + len;
    }
    return false;
}

} // namespace

MdnsBroadcaster::MdnsBroadcaster(QObject *parent)
    : QObject(parent)
{
}

MdnsBroadcaster::~MdnsBroadcaster()
{
    stop();
}

QByteArray MdnsBroadcaster::makeHeader(int anCount, int nsCount, int arCount) const
{
    QByteArray pkt;
    putU16(pkt, 0);                    // ID
    putU16(pkt, 0x8400);               // QR=1 响应, AA=1 权威
    putU16(pkt, 0);                    // QDCOUNT
    putU16(pkt, quint16(anCount));
    putU16(pkt, quint16(nsCount));
    putU16(pkt, quint16(arCount));
    return pkt;
}

QByteArray MdnsBroadcaster::encodeName(const QString &name) const
{
    QByteArray out;
    const QList<QByteArray> labels = name.toLatin1().split('.');
    for (const QByteArray &l : labels) {
        if (l.isEmpty())
            continue;
        if (l.size() > 63)
            return QByteArray();
        out.append(char(quint8(l.size())));
        out.append(l);
    }
    out.append(char(0));
    return out;
}

QString MdnsBroadcaster::serviceFullName(const Service &svc) const
{
    return QStringLiteral("%1.%2.local").arg(svc.name, svc.type);
}

QByteArray MdnsBroadcaster::buildTxtRdata(const Service &svc) const
{
    QByteArray rdata;
    for (const auto &kv : svc.txt) {
        const QByteArray item = kv.first.toUtf8() + "=" + kv.second.toUtf8();
        if (item.size() > 255)
            continue;
        rdata.append(char(quint8(item.size())));
        rdata.append(item);
    }
    return rdata;
}

void MdnsBroadcaster::appendAnswer(QByteArray &pkt, int &anCount,
                                   const QString &name, quint16 type,
                                   quint32 ttl, const QByteArray &rdata) const
{
    pkt.append(encodeName(name));
    putU16(pkt, type);
    // class IN; 非 PTR 记录必须置缓存刷除位(RFC 6762 §10.2),
    // 否则 iOS 会把 SRV/TXT/A 当作"共享记录"合并缓存, 响应被降权处理
    putU16(pkt, type == kTypePTR ? 1 : 0x8001);
    putU32(pkt, ttl);
    putU16(pkt, quint16(rdata.size()));
    pkt.append(rdata);
    ++anCount;
}

void MdnsBroadcaster::sendAnswerForService(const QString &fullName, const Service &svc,
                                           QByteArray &pkt, int &anCount,
                                           bool withPtr, quint16 qtype)
{
    Q_UNUSED(qtype)   // 无论查什么类型, 始终回完整记录集:
                     // iOS 点击投屏后会发 SRV/TXT/A 查询拿连接信息,
                     // 若响应只含部分记录(iOS 还要等下一次查询/announce 补全)就会转圈数秒。
    const QString typeName = svc.type + ".local";
    const QByteArray txtRdata = buildTxtRdata(svc);
    const QByteArray hostEnc = encodeName(m_hostName);

    if (withPtr)
        appendAnswer(pkt, anCount, typeName, kTypePTR, kTtlService,
                     encodeName(fullName));
    {
        QByteArray rdata;
        putU16(rdata, 0);              // priority
        putU16(rdata, 0);              // weight
        putU16(rdata, svc.port);
        rdata.append(hostEnc);
        appendAnswer(pkt, anCount, fullName, kTypeSRV, kTtlService, rdata);
    }
    appendAnswer(pkt, anCount, fullName, kTypeTXT, kTtlService, txtRdata);
    {
        QByteArray a;
        const quint32 ip = m_ipv4.toIPv4Address();
        a.append(char((ip >> 24) & 0xff));
        a.append(char((ip >> 16) & 0xff));
        a.append(char((ip >> 8) & 0xff));
        a.append(char(ip & 0xff));
        appendAnswer(pkt, anCount, m_hostName, kTypeA, kTtlHost, a);
    }
}

void MdnsBroadcaster::buildQueryResponse(quint16 qtype, const QString &qname,
                                         QByteArray &pkt, int &anCount)
{
    if (qname == m_hostName) {
        if (qtype == kTypeA || qtype == kTypeANY) {
            QByteArray a;
            const quint32 ip = m_ipv4.toIPv4Address();
            a.append(char((ip >> 24) & 0xff));
            a.append(char((ip >> 16) & 0xff));
            a.append(char((ip >> 8) & 0xff));
            a.append(char(ip & 0xff));
            appendAnswer(pkt, anCount, m_hostName, kTypeA, kTtlHost, a);
        }
        return;
    }

    // 服务发现查询:返回本机广播的所有服务类型(iOS 打开控制中心靠它枚举服务)
    if (qname == kServicesDnsSdName) {
        const QString servicesName = kServicesDnsSdName;
        for (const Service &svc : m_services) {
            appendAnswer(pkt, anCount, servicesName, kTypePTR, kTtlService,
                         encodeName(svc.type + ".local"));
        }
        return;
    }

    for (const Service &svc : m_services) {
        const QString fullName = serviceFullName(svc);
        const QString typeName = svc.type + ".local";
        if (qname == typeName) {
            // 按类型广播的 PTR 查询 → 应答该服务完整记录
            sendAnswerForService(fullName, svc, pkt, anCount, true, qtype);
        } else if (qname == fullName) {
            sendAnswerForService(fullName, svc, pkt, anCount, false, qtype);
        }
    }
}

void MdnsBroadcaster::onReadyRead()
{
    if (!m_sock)
        return;

    while (m_sock->hasPendingDatagrams()) {
        QByteArray pkt;
        QHostAddress from;
        quint16 fromPort = 0;
        pkt.resize(int(m_sock->pendingDatagramSize()));
        m_sock->readDatagram(pkt.data(), pkt.size(), &from, &fromPort);
        if (pkt.size() < 12)
            continue;

        const int qdCount = (quint8(pkt[4]) << 8) | quint8(pkt[5]);
        if (qdCount <= 0)
            continue;

        // 解析全部 question(RFC 6762: 查询包可能含多个问题, 需全部应答)
        QByteArray resp;
        int anCount = 0;
        int pos = 12;
        for (int q = 0; q < qdCount; ++q) {
            QString qname;
            if (!decodeName(pkt, pos, qname))
                break;
            if (pos + 4 > pkt.size())
                break;
            const quint16 qtype = (quint8(pkt[pos]) << 8) | quint8(pkt[pos + 1]);
            const quint16 qclass = (quint8(pkt[pos + 2]) << 8) | quint8(pkt[pos + 3]);
            pos += 4;
            buildQueryResponse(qtype, qname, resp, anCount);
        }
        if (anCount > 0) {
            resp = makeHeader(anCount) + resp;
            // 双保险:组播应答 + 单播回查询源。iOS 打开控制中心/点击投屏时的
            // 查询若响应包被路由器组播过滤或丢包, iOS 会指数退避重查
            // (1s/3s/9s), 表现为手机转圈数秒。双发保证至少一份到达。
            sendPacket(resp, QHostAddress(QStringLiteral("224.0.0.251")), kMdnsPort);
            sendPacket(resp, from, fromPort);
            qInfo() << "[mdns] reply" << qdCount << "q, answers=" << anCount
                    << "mcast+ucast"
                    << "from" << from.toString();
        }
    }
}

void MdnsBroadcaster::sendPacket(const QByteArray &pkt, const QHostAddress &to, quint16 port)
{
    if (m_sock)
        m_sock->writeDatagram(pkt, to, port);
}

void MdnsBroadcaster::announce()
{
    // 主动广告:每个服务发一个包含 PTR/SRV/TXT/A 的响应包到组播
    for (const Service &svc : m_services) {
        QByteArray pkt;
        int anCount = 0;
        sendAnswerForService(serviceFullName(svc), svc, pkt, anCount, true, kTypeANY);
        if (anCount > 0)
            sendPacket(makeHeader(anCount) + pkt, QHostAddress(QStringLiteral("224.0.0.251")), kMdnsPort);
    }
}

bool MdnsBroadcaster::start(const QString &hostName, const QHostAddress &ipv4,
                            const QList<Service> &services)
{
    stop();

    if (ipv4.isNull() || hostName.isEmpty() || services.isEmpty())
        return false;

    m_hostName = hostName;
    m_ipv4 = ipv4;
    m_services = services;

    m_sock = new QUdpSocket(this);
    if (!m_sock->bind(QHostAddress::AnyIPv4, kMdnsPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "[mdns] bind 5353 failed:" << m_sock->errorString();
        m_sock->deleteLater();
        m_sock = nullptr;
        return false;
    }
    // 组播 join 到广播 IP 所在接口(多网卡时保证收到 iPhone 的查询)
    const QHostAddress groupAddr(QStringLiteral("224.0.0.251"));
    QNetworkInterface iface;
    const auto allIfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &i : allIfaces) {
        for (const QNetworkAddressEntry &e : i.addressEntries()) {
            if (e.ip() == m_ipv4) {
                iface = i;
                break;
            }
        }
        if (iface.isValid())
            break;
    }
    if (iface.isValid())
        m_sock->joinMulticastGroup(groupAddr, iface);
    else
        m_sock->joinMulticastGroup(groupAddr);
    connect(m_sock, &QUdpSocket::readyRead, this, &MdnsBroadcaster::onReadyRead);

    announce();
    // 周期 announce 兜底:错过启动广播、或查询响应被路由器组播过滤时,
    // iOS 也能在 3s 内被动收到服务记录(搜索列表/连接前确认都能快速完成)
    m_announceTimer = new QTimer(this);
    m_announceTimer->setInterval(2000);
    connect(m_announceTimer, &QTimer::timeout, this, &MdnsBroadcaster::announce);
    m_announceTimer->start();

    m_started = true;
    qInfo() << "[mdns] broadcasting" << m_hostName << m_ipv4.toString();
    return true;
}

void MdnsBroadcaster::stop()
{
    if (m_announceTimer) {
        m_announceTimer->stop();
        m_announceTimer->deleteLater();
        m_announceTimer = nullptr;
    }
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->close();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
    m_started = false;
}

} // namespace mirror
