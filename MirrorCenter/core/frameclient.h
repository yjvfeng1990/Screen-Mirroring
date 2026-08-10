#pragma once

#include <QObject>
#include <QImage>
#include <QHostAddress>
#include <QPointer>

class QTcpSocket;

namespace mirror {

/**
 * 帧接收客户端(core 库,无 UI)。
 * 连接 UWP Miracast 接收进程的 TCP 帧服务器,接收 BGRA8 帧。
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

    void connectToServer(const QHostAddress &addr, quint16 port);
    void disconnectFromServer();

    /// 最近一帧(未就绪返回 null)
    QImage latestFrame() const;

    /// 视频尺寸(连接后填充)
    QSize videoSize() const;

signals:
    void connected();
    void disconnected();
    void frameReady();

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();

private:
    bool tryParseFrame();

    QPointer<QTcpSocket> m_socket;
    QByteArray m_buffer;
    bool m_headerParsed = false;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    int m_payloadSize = 0;
    QImage m_latestFrame;
    QSize m_videoSize;
};

} // namespace mirror
