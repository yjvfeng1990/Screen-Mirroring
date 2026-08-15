#pragma once

#include <QObject>
#include <QImage>
#include <QHostAddress>
#include <QPointer>

class QTcpServer;
class QTcpSocket;

namespace mirror {

/**
 * 帧接收服务端(core 库,无 UI)。
 * 本机监听端口,等待 UWP Miracast 接收进程主动连接(出站方是 UWP,
 * 因为 AppContainer 禁止外部进程入站连接其监听端口,而 loopback 豁免
 * 允许 UWP 出站访问 localhost)。连接后接收 BGRA8 帧。
 *
 * 帧协议(与 UWP 侧一致):
 *   [8B magic "MCVIDEO0"] [4B width] [4B height] [4B stride] [4B size] [payload BGRA8]
 */
class FrameClient : public QObject
{
    Q_OBJECT
public:
    explicit FrameClient(QObject *parent = nullptr);
    ~FrameClient() override;

    /// 在本机监听端口(等待 UWP 连接)。失败时发出 connected() 为 false 语义的日志。
    void startListening(quint16 port);
    void stopListening();

    /// 最近一帧(未就绪返回 null)
    QImage latestFrame() const;

    /// 视频尺寸(连接后填充)
    QSize videoSize() const;

signals:
    /// 监听就绪(可接受连接)
    void connected();
    /// 帧通道已建立(UWP 连入)
    void clientConnected();
    void disconnected();
    void frameReady();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onClientDisconnected();

private:
    bool tryParseFrame();

    QPointer<QTcpServer> m_server;
    QPointer<QTcpSocket> m_socket;
    QByteArray m_buffer;
    bool m_headerParsed = false;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    int m_payloadSize = 0;
    QImage m_latestFrame;
    QSize m_videoSize;
    quint64 m_framesReceived = 0;
    // 双缓冲:复用两块 QImage, 避免每帧 8MB(1080p BGRA)分配+释放。
    // tryParseFrame 写到"非显示中"的 buffer, 再整体换手给 m_latestFrame(隐式共享,零拷贝)。
    QImage m_ping, m_pong;
};

} // namespace mirror
