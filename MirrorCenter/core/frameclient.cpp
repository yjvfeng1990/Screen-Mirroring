#include "frameclient.h"

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
    disconnectFromServer();
}

void FrameClient::connectToServer(const QHostAddress &addr, quint16 port)
{
    disconnectFromServer();
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &FrameClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &FrameClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &FrameClient::onReadyRead);
    m_socket->connectToHost(addr, port);
}

void FrameClient::disconnectFromServer()
{
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_buffer.clear();
    m_headerParsed = false;
    m_width = m_height = m_stride = m_payloadSize = 0;
    m_videoSize = QSize();
}

QImage FrameClient::latestFrame() const
{
    return m_latestFrame;
}

QSize FrameClient::videoSize() const
{
    return m_videoSize;
}

void FrameClient::onConnected()
{
    m_buffer.clear();
    m_headerParsed = false;
    emit connected();
}

void FrameClient::onDisconnected()
{
    m_latestFrame = QImage();
    m_videoSize = QSize();
    m_headerParsed = false;
    emit disconnected();
}

void FrameClient::onReadyRead()
{
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

    // 拷贝帧数据(BGRA8)
    const uchar *payload = reinterpret_cast<const uchar *>(
        m_buffer.constData() + kHeaderSize);
    QImage img(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
    const int dstStride = img.bytesPerLine();
    const uchar *src = payload;
    uchar *dst = img.bits();
    const int copyBytes = qMin(m_stride, dstStride);
    for (int y = 0; y < m_height; ++y) {
        memcpy(dst + y * dstStride, src + y * m_stride, copyBytes);
    }
    m_latestFrame = img;
    m_videoSize = QSize(m_width, m_height);

    // 移除已消费数据
    m_buffer.remove(0, kHeaderSize + m_payloadSize);
    m_headerParsed = false;

    emit frameReady();
    return true;
}

} // namespace mirror
