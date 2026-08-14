#pragma once

#include <QObject>
#include <QByteArray>
#include <QHostAddress>

class QTcpSocket;

namespace mirror {

/**
 * MS-MICE 的 RTSP 协商引擎。
 *
 * MS-MICE 中角色反转:Source 是 RTSP 服务器(监听其 RTSP 端口),
 * Sink 作为 RTSP 客户端主动连接 Source 的 RTSP 端口并主导部分协商。
 *
 * 协商流程(对应 WFD M1-M7,可容忍 Source 主动插入请求):
 *   M1: Sink → OPTIONS *            (确认能力, Source 回 Public)
 *   M5: Sink → SET_PARAMETER wfd_client_rtp_ports (告知 Sink 的 RTP 接收端口)
 *   M7: Sink → SETUP                 (建立媒体会话)
 *   Source 主动发来的 OPTIONS/GET_PARAMETER/SET_PARAMETER 请求自动响应。
 */
class MiceRtsp : public QObject
{
    Q_OBJECT
public:
    explicit MiceRtsp(QObject *parent = nullptr);
    ~MiceRtsp() override;

    /**
     * 启动协商。
     * @param sourceIp      Source 的 IP(与 7250 连接同一来源)
     * @param sourceRtspPort Source 监听的 RTSP 端口(SOURCE_READY 的 RTSP_PORT TLV)
     * @param clientRtpPort  Sink 为接收视频流监听的 UDP RTP 端口
     */
    void startNegotiation(const QHostAddress &sourceIp, quint16 sourceRtspPort,
                          quint16 clientRtpPort);
    void teardown();

signals:
    void negotiated();                       // 协商完成, 等待 RTP 流
    void failed(const QString &why);
    void remoteClosed();

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onError();

private:
    void handleMessage(const QByteArray &head, const QByteArray &first,
                       const QByteArray &body);
    void handleRequest(const QByteArray &head, const QByteArray &requestLine);
    void handleResponse(const QByteArray &head, const QByteArray &statusLine);
    QByteArray buildRequest(const QString &method, const QString &uri,
                            const QByteArray &body = QByteArray());
    void sendClientRtpPorts();
    void sendData(const QByteArray &data);

    QTcpSocket *m_sock = nullptr;
    QByteArray m_buf;
    quint16 m_clientRtpPort = 0;
    int m_cseq = 1;
    int m_state = 0;   // 0=未连接 1=OPTIONS 已发 2=client_rtp_ports 已发 3=SETUP 已发 4=已协商
    QByteArray m_sessionId;
};

} // namespace mirror
