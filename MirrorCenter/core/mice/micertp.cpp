#include "micertp.h"

#include <QUdpSocket>
#include <QDebug>

namespace mirror {

namespace {
constexpr int kMaxRtpPacket = 2048;

/* RTP 头字段 */
quint16 be16(const char *p) { return (quint16(quint8(p[0])) << 8) | quint8(p[1]); }
quint32 be32(const char *p)
{
    return (quint32(quint8(p[0])) << 24) | (quint32(quint8(p[1])) << 16)
         | (quint32(quint8(p[2])) << 8) | quint8(p[3]);
}
} // namespace

MiceRtpReceiver::MiceRtpReceiver(QObject *parent)
    : QObject(parent)
{
}

MiceRtpReceiver::~MiceRtpReceiver()
{
    stop();
}

quint16 MiceRtpReceiver::start(quint16 preferred)
{
    stop();

    // 偶数端口作为 RTP(以 2 对齐,兼容 RTCP=+1 的常见约定)
    if (preferred == 0 || (preferred & 1))
        preferred = 41000;

    for (int attempt = 0; attempt < 200; ++attempt) {
        const quint16 candidate = preferred + attempt * 2;
        auto *rtp = new QUdpSocket(this);
        auto *rtcp = new QUdpSocket(this);
        if (rtp->bind(QHostAddress::AnyIPv4, candidate) &&
            rtcp->bind(QHostAddress::AnyIPv4, candidate + 1)) {
            m_rtpSock = rtp;
            m_rtcpSock = rtcp;
            m_rtpPort = candidate;
            connect(m_rtpSock, &QUdpSocket::readyRead, this, &MiceRtpReceiver::onRtpReadyRead);
            // RTCP 端口丢弃即可,无需连接 readyRead
            qInfo() << "[mice-rtp] listening on UDP" << candidate
                    << "(rtcp" << candidate + 1 << ")";
            return candidate;
        }
        delete rtp;
        delete rtcp;
    }
    qWarning() << "[mice-rtp] failed to bind RTP port near" << preferred;
    return 0;
}

void MiceRtpReceiver::stop()
{
    if (m_rtpSock) {
        m_rtpSock->disconnect(this);
        m_rtpSock->close();
        m_rtpSock->deleteLater();
        m_rtpSock = nullptr;
    }
    if (m_rtcpSock) {
        m_rtcpSock->close();
        m_rtcpSock->deleteLater();
        m_rtcpSock = nullptr;
    }
    m_rtpPort = 0;
    m_sps.clear();
    m_pps.clear();
    m_fuActive = false;
    m_fuBuf.clear();
    m_haveLastSeq = false;
}

void MiceRtpReceiver::onRtpReadyRead()
{
    if (!m_rtpSock)
        return;
    while (m_rtpSock->hasPendingDatagrams()) {
        QByteArray pkt;
        pkt.resize(int(m_rtpSock->pendingDatagramSize()));
        m_rtpSock->readDatagram(pkt.data(), pkt.size());
        if (pkt.size() >= 12)
            processPacket(pkt);
    }
}

void MiceRtpReceiver::processPacket(const QByteArray &pkt)
{
    const char *p = pkt.constData();
    const int version = (p[0] >> 6) & 0x3;
    if (version != 2)
        return;
    const int cc = p[0] & 0x0f;
    const bool hasExt = (p[0] & 0x10) != 0;
    const quint16 seq = be16(p + 2);

    // 丢包检测(仅同会话内连续性)
    if (m_haveLastSeq && seq != quint16(m_lastSeq + 1)) {
        if (m_fuActive) {
            m_fuActive = false;
            m_fuBuf.clear();
        }
        qDebug() << "[mice-rtp] packet loss detected, seq" << m_lastSeq << "->" << seq;
    }
    m_lastSeq = seq;
    m_haveLastSeq = true;

    int off = 12 + cc * 4;
    if (hasExt) {
        if (off + 4 > pkt.size())
            return;
        const int extLen = be16(p + off + 2) * 4;
        off += 4 + extLen;
    }
    if (off >= pkt.size())
        return;

    const int nalType = quint8(p[off]) & 0x1f;
    const QByteArray payload = pkt.mid(off);

    if (nalType == 24) { // STAP-A: 聚合多个完整 NALU
        int i = 1;
        while (i + 2 <= payload.size()) {
            const int naluLen = be16(payload.constData() + i);
            i += 2;
            if (i + naluLen > payload.size())
                break;
            emitFrame(payload.mid(i, naluLen));
            i += naluLen;
        }
    } else if (nalType == 28) { // FU-A: 分片
        if (payload.size() < 2)
            return;
        const quint8 fuHeader = quint8(payload[1]);
        const bool start = (fuHeader & 0x80) != 0;
        const bool end = (fuHeader & 0x40) != 0;
        const int realType = fuHeader & 0x1f;
        // 重建 NAL header: FU indicator 高 3 位 + 真实类型
        const char nalHeader = char((quint8(payload[0]) & 0xe0) | realType);

        if (start) {
            m_fuActive = true;
            m_fuBuf.clear();
            m_fuBuf.append(nalHeader);
        }
        if (m_fuActive) {
            if (payload.size() > 2)
                m_fuBuf.append(payload.constData() + 2, payload.size() - 2);
            if (end) {
                m_fuActive = false;
                emitFrame(m_fuBuf);
                m_fuBuf.clear();
            }
        }
    } else if (nalType < 24) { // 单 NALU
        emitFrame(payload);
    }
    // 26/27/29 (STAP-B/MTAP) 等在本实现中不支持,直接忽略
}

void MiceRtpReceiver::appendStartCode(QByteArray &out)
{
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x01));
}

void MiceRtpReceiver::emitFrame(const QByteArray &nalu)
{
    if (nalu.isEmpty())
        return;

    // 缓存参数集
    const int type = quint8(nalu[0]) & 0x1f;
    if (type == 7)
        m_sps = nalu;
    else if (type == 8)
        m_pps = nalu;

    QByteArray frame;
    if (type == 5 && (!m_sps.isEmpty() || !m_pps.isEmpty())) {
        // IDR 帧前置 SPS/PPS,保证解码器参数完整
        if (!m_sps.isEmpty()) {
            appendStartCode(frame);
            frame.append(m_sps);
        }
        if (!m_pps.isEmpty()) {
            appendStartCode(frame);
            frame.append(m_pps);
        }
    }
    appendStartCode(frame);
    frame.append(nalu);
    emit annexBFrame(frame);
}

} // namespace mirror
