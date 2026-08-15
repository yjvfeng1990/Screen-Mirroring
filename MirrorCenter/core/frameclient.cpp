#include "frameclient.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QDataStream>
#include <QDebug>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

namespace mirror {

namespace {
const char kMagic[8] = { 'M', 'C', 'V', 'I', 'D', 'E', 'O', '0' };
// v2 协议头: [magic 8][w 4][h 4][stride 4][size 4][slot 4] = 28B, 负载走共享内存
const int kHeaderSize = 28;
// 与 MiracastReceiverService 严格一致: 每槽 1080p BGRA8 上限, 双槽轮换
const int kMaxSlotBytes = 1920 * 1080 * 4;
// v2.1 seqlock: 槽尾 4B state(奇数=写入中, 偶数=完整, 单调递增)。
// 宿主 memcpy 前后各读一次, 两次相同且为偶数才采信, 否则丢帧 → 杜绝
// 读到"写了一半"的混合数据(偶现花屏根因)。
const int kSlotStateOff = kMaxSlotBytes - 4;

inline int readShmState(const void *slotBase)
{
    int v = 0;
    memcpy(&v, static_cast<const char *>(slotBase) + kSlotStateOff, sizeof(v));
    return v;
}

#ifdef _WIN32
void *mapFrameShm(quint16 port)
{
    wchar_t name[64];
    swprintf_s(name, 64, L"Local\\MirrorCenterFrames_%u", static_cast<unsigned>(port));
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) {
        qWarning() << "[frame] OpenFileMappingW failed:" << GetLastError()
                   << "(shm name" << QString::fromWCharArray(name) << ")";
        return nullptr;
    }
    void *p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(h);
    if (!p)
        qWarning() << "[frame] MapViewOfFile failed:" << GetLastError();
    return p;
}

void unmapFrameShm(void *base)
{
    if (base)
        UnmapViewOfFile(base);
}
#endif
} // namespace

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
    m_port = port;
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
    m_width = m_height = m_stride = m_payloadSize = m_slot = 0;
    m_videoSize = QSize();
    m_latestFrame = QImage();
#ifdef _WIN32
    if (m_shmBase) {
        unmapFrameShm(m_shmBase);
        m_shmBase = nullptr;
    }
#endif
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
    m_width = m_height = m_stride = m_payloadSize = m_slot = 0;
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
#ifdef _WIN32
    if (m_shmBase) {
        unmapFrameShm(m_shmBase);
        m_shmBase = nullptr;
    }
#endif
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

void FrameClient::setTargetFps(int fps)
{
    if (!m_socket || !m_socket->isValid())
        return;
    QByteArray cmd = "SETFPS " + QByteArray::number(fps) + "\n";
    m_socket->write(cmd);
    m_socket->flush();
    qInfo() << "[frame] send SETFPS" << fps << "(port" << m_port << ")";
}

void FrameClient::setTargetEdge(int edge)
{
    if (!m_socket || !m_socket->isValid())
        return;
    QByteArray cmd = "SETEDGE " + QByteArray::number(edge) + "\n";
    m_socket->write(cmd);
    m_socket->flush();
    qInfo() << "[frame] send SETEDGE" << edge << "(port" << m_port << ")";
}

void FrameClient::onReadyRead()
{
    if (!m_socket)
        return;
    m_buffer.append(m_socket->readAll());
    while (tryParseFrame())
        ;
    // 压缩消费掉的前缀:仅在偏移过半时做一次 memmove, 避免每帧 8MB 移动
    if (m_bufferOffset > 0 && m_bufferOffset >= m_buffer.size() / 2) {
        m_buffer.remove(0, m_bufferOffset);
        m_bufferOffset = 0;
    }
}

bool FrameClient::tryParseFrame()
{
    const int avail = m_buffer.size() - m_bufferOffset;
    // 解析头部
    if (!m_headerParsed) {
        if (avail < kHeaderSize)
            return false;
        if (memcmp(m_buffer.constData() + m_bufferOffset, kMagic, 8) != 0) {
            // 数据不同步,丢弃一个字节重试
            ++m_bufferOffset;
            if (m_bufferOffset > 4096) {
                m_buffer.remove(0, m_bufferOffset);
                m_bufferOffset = 0;
            }
            return true;
        }
        QDataStream ds(QByteArray::fromRawData(
            m_buffer.constData() + m_bufferOffset + 8, 20));
        ds.setByteOrder(QDataStream::LittleEndian);
        ds >> m_width >> m_height >> m_stride >> m_payloadSize >> m_slot;
        m_headerParsed = true;
    }

    // v2: 负载在共享内存, TCP 头即整帧, 无需等待负载字节
#ifdef _WIN32
    if (!m_shmBase) {
        // 服务端创建共享映射在首个帧之前, 首个头到达时必然已就绪; 失败则丢帧下一帧再试
        m_shmBase = mapFrameShm(m_port);
        if (!m_shmBase) {
            m_bufferOffset += kHeaderSize;
            m_headerParsed = false;
            return true;
        }
        qInfo() << "[frame] shared memory mapped (port" << m_port << ")";
    }
#else
    // 非 Windows 无共享内存通道: 丢帧
    m_bufferOffset += kHeaderSize;
    m_headerParsed = false;
    return true;
#endif

    // 从共享内存取负载。协议:stride==0 → JPEG(旧式); stride==w*4 → RAW BGRA8。
    // 注意:RAW 用 Format_RGB32:little-endian 下其内存字节序恰为 B-G-R-X,
    // 与 BGRA 匹配;且不预乘 alpha(避免 D3D surface alpha=0 时按
    // ARGB32_Premultiplied 解释导致整体花屏/透明)。
    const uchar *payload = reinterpret_cast<const uchar *>(m_shmBase)
                           + static_cast<qint64>(m_slot) * kMaxSlotBytes;
    // v2.1 seqlock 校验: 读 state → memcpy → 再读 state, 两次相同且为偶数才采信。
    // 服务端写槽期间 state=奇数(写入中), 完整后=偶数; 若 memcpy 期间槽被
    // 下一轮覆盖, 校验失败 → 丢弃本帧(宁丢不花)。
    const int seq0 = readShmState(payload);
    if (seq0 == 0 || (seq0 & 1)) {
        // 尚未初始化或写入中: 丢帧, 下一帧头再来
        m_bufferOffset += kHeaderSize;
        m_headerParsed = false;
        return true;
    }
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
            m_bufferOffset += kHeaderSize;
            m_headerParsed = false;
            return true;
        }
    } else {
        const int dstStride = back->bytesPerLine();
        if (m_stride == dstStride) {
            // stride 一致:整块一次 memcpy(1080p 8MB, 避免逐行 1080 次调用)
            memcpy(back->bits(), payload, static_cast<size_t>(m_height) * m_stride);
        } else {
            const uchar *src = payload;
            uchar *dst = back->bits();
            const int copyBytes = qMin(m_stride, dstStride);
            for (int y = 0; y < m_height; ++y) {
                memcpy(dst + y * dstStride, src + y * m_stride, copyBytes);
            }
        }
    }
    // 写完成后校验: state 必须与 memcpy 前一致且仍为偶数
    if (readShmState(payload) != seq0) {
        m_bufferOffset += kHeaderSize;
        m_headerParsed = false;
        return true;
    }
    if (back->isNull()) {
        // 解码失败:丢弃这一帧,继续等下一帧(避免坏帧卡住链路)
        m_bufferOffset += kHeaderSize;
        m_headerParsed = false;
        return true;
    }
    m_latestFrame = *back;   // 隐式共享换手,零拷贝
    m_videoSize = QSize(m_width, m_height);

    // 移除已消费数据(仅移动偏移, 实际内存由 onReadyRead 统一压缩)
    m_bufferOffset += kHeaderSize;
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
