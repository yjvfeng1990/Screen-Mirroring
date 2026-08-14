#include "micertsp.h"

#include <QTcpSocket>
#include <QTimer>
#include <QDebug>

namespace mirror {

namespace {
constexpr int kNegotiationTimeoutMs = 10000;

/* 提取头字段 "Name: value" */
QByteArray headerValue(const QByteArray &headers, const QByteArray &name)
{
    const int idx = headers.indexOf(name + ":");
    if (idx < 0)
        return QByteArray();
    int start = idx + name.size() + 1;
    while (start < headers.size() && headers[start] == ' ')
        ++start;
    int end = headers.indexOf("\r\n", start);
    if (end < 0)
        end = headers.size();
    return headers.mid(start, end - start);
}
} // namespace

MiceRtsp::MiceRtsp(QObject *parent)
    : QObject(parent)
{
}

MiceRtsp::~MiceRtsp()
{
    teardown();
}

void MiceRtsp::teardown()
{
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
    m_buf.clear();
    m_state = 0;
    m_cseq = 1;
    m_sessionId.clear();
}

void MiceRtsp::startNegotiation(const QHostAddress &sourceIp, quint16 sourceRtspPort,
                                quint16 clientRtpPort)
{
    teardown();
    m_clientRtpPort = clientRtpPort;
    m_state = 1;   // 连接建立后发 OPTIONS

    m_sock = new QTcpSocket(this);
    connect(m_sock, &QTcpSocket::connected, this, &MiceRtsp::onConnected);
    connect(m_sock, &QTcpSocket::readyRead, this, &MiceRtsp::onReadyRead);
    connect(m_sock, &QTcpSocket::disconnected, this, &MiceRtsp::onDisconnected);
    connect(m_sock, &QTcpSocket::errorOccurred, this, &MiceRtsp::onError);
    m_sock->connectToHost(sourceIp, sourceRtspPort);

    QTimer::singleShot(kNegotiationTimeoutMs, this, [this] {
        if (m_state != 0 && m_state != 4) {
            qWarning() << "[mice-rtsp] negotiation timeout";
            emit failed(QStringLiteral("RTSP negotiation timeout"));
            teardown();
        }
    });
}

void MiceRtsp::onConnected()
{
    qInfo() << "[mice-rtsp] connected to source";
    sendData(buildRequest(QStringLiteral("OPTIONS"), QStringLiteral("*")));
}

void MiceRtsp::onDisconnected()
{
    if (m_state == 4) {
        emit remoteClosed();
    } else {
        emit failed(QStringLiteral("RTSP connection closed during negotiation"));
    }
    teardown();
}

void MiceRtsp::onError()
{
    if (!m_sock)
        return;
    qWarning() << "[mice-rtsp] socket error:" << m_sock->errorString();
    emit failed(QStringLiteral("RTSP connect failed: %1").arg(m_sock->errorString()));
    teardown();
}

void MiceRtsp::onReadyRead()
{
    if (!m_sock)
        return;
    m_buf.append(m_sock->readAll());

    // RTSP 消息以空行分头/体;循环处理多条
    while (true) {
        const int sep = m_buf.indexOf("\r\n\r\n");
        if (sep < 0)
            return; // 头未完整
        QByteArray head = m_buf.left(sep);
        m_buf.remove(0, sep + 4);

        const QByteArray cl = headerValue(head, "Content-Length");
        int bodyLen = cl.isEmpty() ? 0 : cl.toInt();
        if (bodyLen > 0 && m_buf.size() < bodyLen)
            return; // 体未完整
        QByteArray body = m_buf.left(bodyLen);
        m_buf.remove(0, bodyLen);

        const int nl = head.indexOf("\r\n");
        QByteArray first = (nl >= 0) ? head.left(nl) : QByteArray(); // 请求行或状态行
        handleMessage(head, first, body);
    }
}

void MiceRtsp::handleMessage(const QByteArray &head, const QByteArray &first,
                             const QByteArray &body)
{
    Q_UNUSED(body)
    if (first.startsWith("RTSP/")) {
        handleResponse(head, first);
    } else {
        handleRequest(head, first);
    }
}

/* ---------------- 响应处理(对 M2/M4/M6/M8) ---------------- */
void MiceRtsp::handleResponse(const QByteArray &head, const QByteArray &statusLine)
{
    const int code = statusLine.mid(9, 3).toInt();
    const QByteArray cseq = headerValue(head, "CSeq");
    qInfo() << "[mice-rtsp] response" << statusLine << "cseq=" << cseq;

    if (code != 200) {
        emit failed(QStringLiteral("RTSP error: %1").arg(QString::fromLatin1(statusLine)));
        return;
    }

    switch (m_state) {
    case 1: // M2 → 发 M5 (wfd_client_rtp_ports)
        m_state = 2;
        sendClientRtpPorts();
        break;
    case 2: // M6 → 发 M7 (SETUP)
        m_state = 3;
        sendData(buildRequest(QStringLiteral("SETUP"),
                              QStringLiteral("rtsp://localhost/wfd1.0")));
        break;
    case 3: { // M8 → 协商完成
        m_sessionId = headerValue(head, "Session");
        m_state = 4;
        qInfo() << "[mice-rtsp] negotiated, session=" << m_sessionId
                << "transport=" << headerValue(head, "Transport");
        emit negotiated();
        break;
    }
    default:
        break;
    }
}

/* ---------------- 请求处理(Source 主动发来的请求) ---------------- */
void MiceRtsp::handleRequest(const QByteArray &head, const QByteArray &requestLine)
{
    const QByteArray cseq = headerValue(head, "CSeq");
    const QByteArray method = requestLine.left(requestLine.indexOf(' '));
    qInfo() << "[mice-rtsp] request" << requestLine;

    if (method == "OPTIONS") {
        QByteArray resp =
            "RTSP/1.0 200 OK\r\n"
            "CSeq: " + cseq + "\r\n"
            "Public: org.wfa.wfd1.0, SET_PARAMETER, GET_PARAMETER, OPTIONS\r\n"
            "Content-Length: 0\r\n\r\n";
        sendData(resp);
    } else if (method == "GET_PARAMETER") {
        // Source 探测 Sink 能力;回最简能力集即可
        const QByteArray body =
            "wfd_video_formats: 00 00 00 00 00000001 00000001 00 0000 0000 00 none none\r\n";
        QByteArray resp =
            "RTSP/1.0 200 OK\r\n"
            "CSeq: " + cseq + "\r\n"
            "Content-Type: text/parameters\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        sendData(resp + body);
    } else if (method == "SET_PARAMETER") {
        // 接受 Source 的参数(格式选择等)
        QByteArray resp =
            "RTSP/1.0 200 OK\r\n"
            "CSeq: " + cseq + "\r\n"
            "Content-Length: 0\r\n\r\n";
        sendData(resp);
    } else {
        QByteArray resp =
            "RTSP/1.0 405 Method Not Allowed\r\n"
            "CSeq: " + cseq + "\r\n"
            "Content-Length: 0\r\n\r\n";
        sendData(resp);
    }
}

/* ---------------- 消息组装 ---------------- */
void MiceRtsp::sendClientRtpPorts()
{
    // 偶数端口为 RTP;RTCP 端口为 RTP+1(Source 若发 RTCP 我们忽略)
    const QByteArray body = QByteArrayLiteral("wfd_client_rtp_ports: RTP/AVP/UDP;unicast ")
        + QByteArray::number(m_clientRtpPort) + " "
        + QByteArray::number(m_clientRtpPort + 1) + " mode=play\r\n";
    QByteArray msg =
        "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Content-Type: text/parameters\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
        + body;
    sendData(msg);
}

QByteArray MiceRtsp::buildRequest(const QString &method, const QString &uri,
                                  const QByteArray &body)
{
    QByteArray msg = method.toLatin1() + " " + uri.toLatin1() + " RTSP/1.0\r\n"
        + "CSeq: " + QByteArray::number(m_cseq++) + "\r\n"
        + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        + "User-Agent: MirrorCenter/1.0\r\n\r\n"
        + body;
    return msg;
}

void MiceRtsp::sendData(const QByteArray &data)
{
    if (m_sock)
        m_sock->write(data);
}

} // namespace mirror
