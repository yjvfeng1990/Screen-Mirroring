#pragma once

#include <QObject>
#include <QHash>
#include <QByteArray>
#include <QString>
#include <QHostAddress>

class QTcpServer;
class QTcpSocket;

namespace mirror {

/** MS-MICE Source 会话信息(解析自 SOURCE_READY 消息) */
struct MiceSourceInfo {
    QString friendlyName;      // FRIENDLY_NAME TLV (0x00)
    quint16 rtspPort = 0;      // RTSP_PORT TLV (0x02), Source 监听的 RTSP 端口
    QByteArray sourceId;       // SOURCE_ID TLV (0x03)
    QHostAddress sourceIp;     // 7250 连接的来源 IP(与 RTSP 端口同一主机)
};

/**
 * MS-MICE 控制通道服务器。
 *
 * 职责:
 *   - 监听 TCP 7250,接收 Source 的 MS-MICE 控制消息
 *   - 解析消息头(Size + Version + Command)与 TLVArray
 *   - 处理 SOURCE_READY(0x01) → 发出 sourceReady 信号
 *   - 处理 STOP_PROJECTION(0x02) → 发出 sourceDisconnected 信号
 *
 * 消息格式(MS-MICE 2.2):
 *   Size(2 BE) | Version(1) | Command(1) | TLVArray
 *   TLV: Type(1) | Length(2 BE) | Value
 */
class MiceServer : public QObject
{
    Q_OBJECT
public:
    explicit MiceServer(QObject *parent = nullptr);
    ~MiceServer() override;

    bool start(quint16 port = 7250);
    void stop();
    quint16 port() const { return m_port; }
    bool isListening() const;

    /** 组装一条 MS-MICE 消息(供响应/主动发送用) */
    static QByteArray buildMessage(quint8 command, const QList<QPair<quint8, QByteArray>> &tlvs);

signals:
    void sourceReady(const MiceSourceInfo &info);
    void sourceDisconnected(const QByteArray &sourceId);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void parsePacket(QTcpSocket *sock, const QByteArray &data);

    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
    // 连接 → 半包累积缓冲
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

} // namespace mirror
