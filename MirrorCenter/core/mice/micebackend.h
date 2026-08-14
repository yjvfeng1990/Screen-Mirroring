#pragma once

#include <QObject>
#include <QHash>
#include <QByteArray>
#include <QHostAddress>
#include <QImage>

#include "miceserver.h"

namespace mirror {

class MdnsBroadcaster;
class MiceServer;
class MiceSession;   // 定义于 .cpp

/**
 * MS-MICE(Miracast over Infrastructure)接收端编排器。
 *
 * 链路: mDNS 发布 _display._tcp(container_id) → Source 发现并连 TCP 7250
 *   → SOURCE_READY → Sink 连 Source 的 RTSP 端口协商(M1/M5/M7)
 *   → RTP 收流 → H.264 解码 → frameReady
 *
 * 多路支持: 每个 Source 独立会话(独立 RTP 端口 + 独立解码器),
 * 由 Windows Source 通过 mDNS 依次发现并连入。
 */
class MiceBackend : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString deviceName = QStringLiteral("MirrorCenter");
        QString containerId;              // 空则自动生成 GUID(格式: 大写无花括号)
        quint16 micePort = 7250;          // MS-MICE 控制通道端口
        quint16 rtpBasePort = 41000;      // 每路会话 RTP 端口起始
        int maxSessions = 8;
    };

    explicit MiceBackend(const Config &cfg, QObject *parent = nullptr);
    ~MiceBackend() override;

    bool start();
    void stop();
    bool isRunning() const { return m_running; }
    quint16 micePort() const;
    QString containerId() const;

signals:
    void sourceConnected(const QString &friendlyName, const QByteArray &sourceId);
    void sourceDisconnected(const QByteArray &sourceId);
    void frameReady(const QByteArray &sourceId, const QImage &frame);
    void logMessage(const QString &message);

private slots:
    void onSourceReady(const mirror::MiceSourceInfo &info);
    void onSourceDisconnected(const QByteArray &sourceId);

private:
    QString findLocalIpv4() const;
    QHostAddress findLocalIpv6() const;

    Config m_cfg;
    QString m_containerId;
    MdnsBroadcaster *m_mdns = nullptr;
    MiceServer *m_server = nullptr;
    // session 由本类独占管理
    QHash<QByteArray, MiceSession *> m_sessions;
    QList<quint16> m_rtpPool;
    bool m_running = false;
};

} // namespace mirror
