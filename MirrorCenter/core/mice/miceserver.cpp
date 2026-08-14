#include "miceserver.h"

#include <QTcpServer>
#include <QTcpSocket>

namespace mirror {

MiceServer::MiceServer(QObject *parent)
    : QObject(parent)
{
}

MiceServer::~MiceServer()
{
    stop();
}

bool MiceServer::start(quint16 port)
{
    stop();
    m_server = new QTcpServer(this);
    m_server->setMaxPendingConnections(8);
    connect(m_server, &QTcpServer::newConnection, this, &MiceServer::onNewConnection);
    // 双栈监听: Windows 发送端解析出 IPv6(link-local)后可能走 IPv6 连 7250。
    // Windows 上 AnyIPv6 默认 V6ONLY=0, IPv4 连接同样可达。
    if (!m_server->listen(QHostAddress::AnyIPv6, port)) {
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_port = m_server->serverPort();
    return true;
}

void MiceServer::stop()
{
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    m_buffers.clear();
}

bool MiceServer::isListening() const
{
    return m_server && m_server->isListening();
}

void MiceServer::onNewConnection()
{
    while (QTcpSocket *sock = m_server->nextPendingConnection()) {
        qInfo() << "[mice-server] new connection from" << sock->peerAddress().toString()
                << "port" << sock->peerPort();
        connect(sock, &QTcpSocket::readyRead, this, &MiceServer::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, &MiceServer::onDisconnected);
        m_buffers.insert(sock, QByteArray());
    }
}

void MiceServer::onDisconnected()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock)
        return;
    m_buffers.remove(sock);
    sock->deleteLater();
}

void MiceServer::onReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock)
        return;
    m_buffers[sock].append(sock->readAll());

    // 按 Size 字段切包。[MS-MICE] 2.2.4: Size = Version+Command+TLVArray
    // 的长度(不含 Size 字段本身), 消息总长 = 2 + Size。
    QByteArray &buf = m_buffers[sock];
    while (buf.size() >= 2) {
        const quint16 size = (static_cast<quint8>(buf[0]) << 8) | static_cast<quint8>(buf[1]);
        if (size < 2 || size > 65535)
            break; // 非法,等待断开
        if (buf.size() < 2 + size)
            break; // 半包
        const QByteArray packet = buf.mid(2, size);
        buf.remove(0, 2 + size);
        parsePacket(sock, packet);
    }
}

void MiceServer::parsePacket(QTcpSocket *sock, const QByteArray &data)
{
    if (data.size() < 2)
        return;
    // data 已跳过 Size 字段: data[0]=Version, data[1]=Command, 后续为 TLVArray
    const quint8 version = static_cast<quint8>(data[0]);
    const quint8 command = static_cast<quint8>(data[1]);
    const QByteArray tlvData = data.mid(2);

    MiceSourceInfo info;
    QList<QPair<quint8, QByteArray>> tlvs;

    // 解析 TLVArray
    int off = 0;
    while (off + 3 <= tlvData.size()) {
        const quint8 type = static_cast<quint8>(tlvData[off]);
        const quint16 len = (static_cast<quint8>(tlvData[off + 1]) << 8)
                            | static_cast<quint8>(tlvData[off + 2]);
        off += 3;
        if (off + len > tlvData.size())
            break;
        const QByteArray value = tlvData.mid(off, len);
        off += len;
        tlvs.append({type, value});

        switch (type) {
        case 0x00: // FRIENDLY_NAME
            info.friendlyName = QString::fromUtf8(value);
            break;
        case 0x02: { // RTSP_PORT(兼容 2 字节大端数字 / ASCII 十进制字符串)
            if (value.size() == 2) {
                info.rtspPort = (static_cast<quint8>(value[0]) << 8)
                                | static_cast<quint8>(value[1]);
            } else {
                bool ok = false;
                const int p = value.toInt(&ok);
                if (ok && p > 0 && p <= 65535)
                    info.rtspPort = static_cast<quint16>(p);
            }
            break;
        }
        case 0x03: // SOURCE_ID
            info.sourceId = value;
            break;
        default:
            break;
        }
    }

    switch (command) {
    case 0x01: // SOURCE_READY
        info.sourceIp = sock->peerAddress();
        // 双栈监听下 IPv4 连接的 peer 是 ::ffff:x.x.x.x, 转回 IPv4 便于 RTSP 回连
        if (info.sourceIp.protocol() == QAbstractSocket::IPv6Protocol) {
            const quint32 v4 = info.sourceIp.toIPv4Address();
            if (v4 != 0)
                info.sourceIp = QHostAddress(v4);
        }
        emit sourceReady(info);
        // MS-MICE 2.2.4: Sink 必须回 ACK(0x03), 否则 Source 一直等待握手
        sock->write(buildMessage(0x03, {}));
        break;
    case 0x02: { // STOP_PROJECTION
        // 取 SOURCE_ID 以标识是哪一路
        QByteArray sid;
        for (const auto &t : tlvs)
            if (t.first == 0x03)
                sid = t.second;
        emit sourceDisconnected(sid);
        break;
    }
    default:
        break;
    }
}

QByteArray MiceServer::buildMessage(quint8 command, const QList<QPair<quint8, QByteArray>> &tlvs)
{
    QByteArray body;
    for (const auto &t : tlvs) {
        body.append(static_cast<char>(t.first));
        body.append(static_cast<char>((t.second.size() >> 8) & 0xFF));
        body.append(static_cast<char>(t.second.size() & 0xFF));
        body.append(t.second);
    }
    // Size = Version(1) + Command(1) + TLVArray, 不含 Size 字段本身
    const int total = 2 + body.size();
    QByteArray msg;
    msg.append(static_cast<char>((total >> 8) & 0xFF));
    msg.append(static_cast<char>(total & 0xFF));
    msg.append(static_cast<char>(0x01)); // Version
    msg.append(static_cast<char>(command));
    msg.append(body);
    return msg;
}

} // namespace mirror
