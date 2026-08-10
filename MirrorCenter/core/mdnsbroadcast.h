#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QList>
#include <QPair>
#include <QVector>
#include <QHostAddress>

class QUdpSocket;
class QTimer;

namespace mirror {

/**
 * 自研精简 mDNS 应答器(只做广告与应答,不做探测/缓存)。
 *
 * 用途:网关调度器(单广播名 MirrorCenter + 多静默实例)统一广播
 *   _airplay._tcp 与 _raop._tcp 服务,指向网关的监听端口(7100)。
 *
 * 行为:
 *   - start() 时向组播 224.0.0.251:5353 主动 announce(PTR/SRV/TXT/A)
 *   - 监听 5353 上的查询(PTR/SRV/TXT/A/ANY),匹配后单播/组播应答
 *
 * TXT 字段完全复刻 UxPlay(dnssd_mdnsd.c)的广播内容,保证 iOS 端
 * 识别为标准 AirPlay 接收器(deviceid/features/pk/flags/model 等)。
 */
class MdnsBroadcaster : public QObject
{
    Q_OBJECT
public:
    struct Service {
        QString name;                              // 服务实例名(不含 type),如 "MirrorCenter" 或 "6C6C1B..@MirrorCenter"
        QString type;                              // 如 "_airplay._tcp"
        quint16 port = 0;
        QVector<QPair<QString, QString>> txt;      // TXT 键值(保持顺序)
    };

    explicit MdnsBroadcaster(QObject *parent = nullptr);
    ~MdnsBroadcaster() override;

    /**
     * 启动广播。
     * @param hostName 主机名, 如 "mirrorcenter.local"(A 记录使用)
     * @param ipv4     本机局域网 IPv4
     * @param services 要广播的服务列表
     */
    bool start(const QString &hostName, const QHostAddress &ipv4,
               const QList<Service> &services);
    void stop();

private slots:
    void onReadyRead();

private:
    QByteArray encodeName(const QString &name) const;
    QString serviceFullName(const Service &svc) const;
    QByteArray buildTxtRdata(const Service &svc) const;
    void appendAnswer(QByteArray &pkt, int &anCount, const QString &name,
                      quint16 type, quint32 ttl, const QByteArray &rdata) const;
    void buildQueryResponse(quint16 qtype, const QString &qname,
                            QByteArray &pkt, int &anCount);
    void sendPacket(const QByteArray &pkt, const QHostAddress &to, quint16 port);
    void announce();
    void sendAnswerForService(const QString &fullName, const Service &svc,
                              QByteArray &pkt, int &anCount, bool withPtr,
                              quint16 qtype);
    QByteArray makeHeader(int anCount, int nsCount = 0, int arCount = 0) const;

    QUdpSocket *m_sock = nullptr;
    QTimer *m_announceTimer = nullptr;   // 周期 announce 兜底(错过查询的设备也能看到)
    QString m_hostName;
    QHostAddress m_ipv4;
    QList<Service> m_services;
    bool m_started = false;
};

} // namespace mirror
