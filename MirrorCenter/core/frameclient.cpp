#include "frameclient.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QDataStream>
#include <QDebug>

namespace mirror {

namespace {
const char kMagic[8] = { 'M', 'C', 'V', 'I', 'D', 'E', 'O', '0' };
const int kHeaderSize = 24;
}

FrameClient::FrameClient(QObject *parent)
    : QObject(parent)
{
}

FrameClient::~FrameClient()
{
    stopListening();
}

void FrameClient::startListening(quint16 port)
{
    stopListening();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &FrameClient::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        qWarning() << "[frame] listen failed on port" << port
                   << ":" << m_server->errorString();
        m_server->deleteLater();
        m_server = nullptr;
        return;
    }
    qInfo() << "[frame] listening on 127.0.0.1:" << port;
    emit connected();
}

void FrameClient::stopListening()
{
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_buffer.clear();
    m_headerParsed = false;
    m_width = m_height = m_stride = m_payloadSize = 0;
    m_videoSize = QSize();
    m_latestFrame = QImage();
}

void FrameClient::onNewConnection()
{
    if (!m_server)
        return;
    // 只保留最新一条帧通道;旧连接直接丢弃(UWP 重连时带新端口/会话)
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_socket = m_server->nextPendingConnection();
    if (!m_socket)
        return;
    connect(m_socket, &QTcpSocket::readyRead, this, &FrameClient::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &FrameClient::onClientDisconnected);
    m_buffer.clear();
    m_headerParsed = false;
    m_width = m_height = m_stride = m_payloadSize = 0;
    qInfo() << "[frame] client connected:" << m_socket->peerAddress().toString();
    emit clientConnected();
}

void FrameClient::onClientDisconnected()
{
    qWarning() << "[frame] client disconnected";
    m_latestFrame = QImage();
    m_videoSize = QSize();
    m_headerParsed = false;
    m_buffer.clear();
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    emit disconnected();
}

QImage FrameClient::latestFrame() const
{
    return m_latestFrame;
}

QSize FrameClient::videoSize() const
{
    return m_videoSize;
}

void FrameClient::onReadyRead()
{
    if (!m_socket)
        return;
    m_buffer.append(m_socket->readAll());
    while (tryParseFrame())
        ;
}

bool FrameClient::tryParseFrame()
{
    // 解析头部
    if (!m_headerParsed) {
        if (m_buffer.size() < kHeaderSize)
            return false;
        if (memcmp(m_buffer.constData(), kMagic, 8) != 0) {
            // 数据不同步,丢弃一个字节重试
            m_buffer.remove(0, 1);
            return true;
        }
        QDataStream ds(m_buffer.mid(8, 16));
        ds.setByteOrder(QDataStream::LittleEndian);
        ds >> m_width >> m_height >> m_stride >> m_payloadSize;
        m_headerParsed = true;
    }

    // 等待完整负载
    if (m_buffer.size() < kHeaderSize + m_payloadSize)
        return false;

    // 拷贝帧数据。协议:stride==0 → JPEG(服务端压缩传输,节省 P2P 无线带宽);
    // stride==w*4 → RAW BGRA8(旧版/编码失败回退)。
    // 注意:RAW 用 Format_RGB32:little-endian 下其内存字节序恰为 B-G-R-X,
    // 与 BGRA 匹配;且不预乘 alpha(避免 D3D surface alpha=0 时按
    // ARGB32_Premultiplied 解释导致整体花屏/透明)。
    const uchar *payload = reinterpret_cast<const uchar *>(
        m_buffer.constData() + kHeaderSize);
    // 双缓冲:挑一块"当前未显示"的 QImage 复用(尺寸不变时零分配)。
    // m_latestFrame 可能正被 UI 线程引用(隐式共享), 不能原地写它。
    QImage *back = (m_ping.constBits() == m_latestFrame.constBits()) ? &m_pong : &m_ping;
    if (back->size() != QSize(m_width, m_height)) {
        // 分辨率变化才重建(帧来源切换/投屏端改分辨率)
        back->operator=(QImage(m_width, m_height, QImage::Format_RGB32));
    }
    if (m_stride == 0) {
        // JPEG 压缩帧:解到复用 buffer
        QImage dec;
        if (dec.loadFromData(payload, m_payloadSize)) {
            *back = dec;
        } else {
            m_buffer.remove(0, kHeaderSize + m_payloadSize);
            m_headerParsed = false;
            return true;
        }
    } else {
        const int dstStride = back->bytesPerLine();
        const uchar *src = payload;
        uchar *dst = back->bits();
        const int copyBytes = qMin(m_stride, dstStride);
        for (int y = 0; y < m_height; ++y) {
            memcpy(dst + y * dstStride, src + y * m_stride, copyBytes);
        }
    }
    if (back->isNull()) {
        // 解码失败:丢弃这一帧,继续等下一帧(避免坏帧卡住链路)
        m_buffer.remove(0, kHeaderSize + m_payloadSize);
        m_headerParsed = false;
        return true;
    }
    m_latestFrame = *back;   // 隐式共享换手,零拷贝
    m_videoSize = QSize(m_width, m_height);

    // 移除已消费数据
    m_buffer.remove(0, kHeaderSize + m_payloadSize);
    m_headerParsed = false;

    // 日志节流:每 150 帧输出一次帧率,便于确认链路通且不掉帧
    if (++m_framesReceived == 1 || m_framesReceived % 150 == 0) {
        qInfo() << "[frame] received frame #" << m_framesReceived
                << "size=" << m_width << "x" << m_height;
    }

    emit frameReady();
    return true;
}

} // namespace mirror
