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
 * 本机监听端口,等待 Miracast 接收服务进程(MiracastReceiverService.exe)主动连接。
 * 连接后接收 BGRA8 帧。
 *
 * 帧协议 v2(2026-08-15,与 MiracastReceiverService 一致):
 *   [8B magic "MCVIDEO0"] [4B width] [4B height] [4B stride] [4B size] [4B slot]
 *   负载在共享内存 Local\MirrorCenterFrames_<port> 的双槽中(每槽 1080p BGRA8 上限),
 *   TCP 只承载 28B 头,不再传输 8MB 负载(消除 TCP 回环 + append + memmove 开销)。
 *   slot 0/1 轮换;stride==0 → JPEG(旧式,负载也在共享槽),stride==w*4 → RAW BGRA8。
 */
class FrameClient : public QObject
{
    Q_OBJECT
public:
    explicit FrameClient(QObject *parent = nullptr);
    ~FrameClient() override;

    /// 在本机监听端口(等待接收服务连接)。失败时发出 connected() 为 false 语义的日志。
    void startListening(quint16 port);
    void stopListening();

    /// 最近一帧(未就绪返回 null)
    QImage latestFrame() const;

    /// 视频尺寸(连接后填充)
    QSize videoSize() const;

    /// 全屏放大场景: 向接收服务发送 "SETFPS n"(n>0 强制帧率, 0 恢复默认)。
    /// 焦点路放大时把其它路降到 1fps(连接保持), 缩回后恢复。
    void setTargetFps(int fps);

    /// 混合路数分档: 向接收服务发送 "SETEDGE n"(n>=0 最大边, 0=不缩放)。
    /// 宿主按总活跃路数(含 AirPlay)计算, 覆盖服务端按自身连接数的默认分档。
    void setTargetEdge(int edge);

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
    int m_bufferOffset = 0;   // 已消费前缀偏移(避免每帧 remove 触发 8MB memmove)
    bool m_headerParsed = false;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    int m_payloadSize = 0;
    int m_slot = 0;           // v2: 共享内存槽号(0/1)
    quint16 m_port = 0;       // 本机监听端口(派生共享内存名)
    void *m_shmBase = nullptr; // 共享内存映射基址(负载零 TCP 传输)
    QImage m_latestFrame;
    QSize m_videoSize;
    quint64 m_framesReceived = 0;
    // 双缓冲:复用两块 QImage, 避免每帧 8MB(1080p BGRA)分配+释放。
    // tryParseFrame 写到"非显示中"的 buffer, 再整体换手给 m_latestFrame(隐式共享,零拷贝)。
    QImage m_ping, m_pong;
};

} // namespace mirror
