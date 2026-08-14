#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>

class QUdpSocket;

namespace mirror {

/**
 * MS-MICE RTP 接收器(H.264, RFC 6184 解封装)。
 *
 * 监听本机 UDP 端口,接收 Source 发来的 RTP 流:
 *   - 解析 RTP 头(含 CSRC/扩展头)
 *   - 支持 单 NALU / STAP-A(24) / FU-A(28) 三种打包方式
 *   - 缓存 SPS(7)/PPS(8),在 IDR 帧前自动补发,保证解码器参数完整
 *   - 组出完整 H.264 帧(Annex B)后发出 annexBReady 信号
 *
 * RTCP 端口同时绑定但只丢弃,不处理(模式为单向 play)。
 */
class MiceRtpReceiver : public QObject
{
    Q_OBJECT
public:
    explicit MiceRtpReceiver(QObject *parent = nullptr);
    ~MiceRtpReceiver() override;

    /**
     * 绑定 RTP(偶数)/RTCP(奇数) 端口。
     * @param preferred 期望的 RTP 端口;若被占用自动向上寻找空闲端口
     * @return 实际绑定的 RTP 端口;0 表示失败
     */
    quint16 start(quint16 preferred);
    void stop();
    quint16 rtpPort() const { return m_rtpPort; }

signals:
    /** 完整 H.264 帧(Annex B,可能含 SPS/PPS+IDR) */
    void annexBFrame(const QByteArray &frame);

private slots:
    void onRtpReadyRead();

private:
    void processPacket(const QByteArray &pkt);
    void emitFrame(const QByteArray &nalu);
    static void appendStartCode(QByteArray &out);

    QUdpSocket *m_rtpSock = nullptr;
    QUdpSocket *m_rtcpSock = nullptr;
    quint16 m_rtpPort = 0;

    // H.264 depacketize 状态
    QByteArray m_sps;
    QByteArray m_pps;
    bool m_fuActive = false;
    QByteArray m_fuBuf;         // FU-A 分片累积(不含 start code)
    quint16 m_lastSeq = 0;
    bool m_haveLastSeq = false;
};

} // namespace mirror
