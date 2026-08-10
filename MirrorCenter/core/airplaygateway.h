#pragma once

#include <QObject>
#include <QList>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QHostAddress>

#include "mirrorsession.h"

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace mirror {

class SessionManager;
class MdnsBroadcaster;

/**
 * AirPlay 网关调度器(单广播名 + 多静默实例)。
 *
 * 架构:
 *   iPhone 在投屏列表看到唯一 "MirrorCenter"(mDNS 广播 → TCP 7100)
 *        ↓ 连接 7100
 *   调度代理(AirPlayGateway)
 *        ├─ 转发 → uxplay 实例1 (-silent -p 7201, 静默)
 *        ├─ 转发 → uxplay 实例2 (-silent -p 7301, 静默)
 *        └─ 转发 → uxplay 实例3 (-silent -p 7401, 静默)
 *
 * 要点:
 *   - 所有实例使用相同固定 MAC + 相同 -key 密钥文件, iOS 视为同一台设备,
 *     配对(FairPlay)在所有实例间一致。
 *   - 实例按需动态启动:默认预置 1 个就绪实例;新设备连入无空闲实例时启动新实例。
 *   - 设备断开后实例保留 recycleMs(默认 30s);30s 内同 IP 重连则复用原实例。
 *   - 数据面:视频/音频 RTP 由 iPhone 直连实例 UDP 端口(不需要代理);
 *     仅 NTP 同步回程例外(实例主动向"客户端 IP"发请求,而代理转发后客户端 IP
 *     变为 127.0.0.1),由网关做一条窄 UDP 隧道把 NTP 请求送回 iPhone。
 *   - mDNS 广播由网关自研应答器完成,TXT 中的 pk 取自实例 stdout 输出
 *     (MIRROR_PK=...),保证与实例内部密钥一致。
 */
class AirPlayGateway : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString deviceName = QStringLiteral("MirrorCenter");
        QString backendExe;                  // uxplay.exe 绝对路径
        QString keyfile;                     // 共享 Ed25519 密钥文件(持久化, 保证 pk 一致)
        QString mac = QStringLiteral("6c:6c:1b:30:00:01");  // 固定 MAC(小写冒号格式)
        quint16 gatewayPort = 7100;          // 对外广播与监听端口
        quint16 instanceBasePort = 7201;     // 实例端口基址
        int instanceStep = 100;
        int maxInstances = 16;               // 硬解最多 16 路(软解由 app 侧按 4 格限制)
        int recycleMs = 30000;               // 设备断开后实例保留时间
        QStringList extraArgs;               // 追加实例参数(-vs d3d11videosink 等)
    };

    struct Instance {
        MirrorSession *session = nullptr;
        quint16 basePort = 0;                // -p 参数值
        quint16 rtspPort = 0;                // 实例 RTSP 主端口(默认 base+1, MIRROR_RTSP_PORT 确认)
        QString pk;                          // 实例输出的配对公钥
        QString clientIp;                    // 已绑定设备 IP, 空 = 空闲
        QString clientName;                  // 来源手机名称(MIRROR_CLIENT_NAME)
        QString clientModel;                 // 来源手机型号(MIRROR_CLIENT_MODEL)
        QString decoder;                     // 实际视频解码器(MIRROR_DECODER, 软/硬解判断)
        QPointer<QTcpSocket> clientSock;     // -> iPhone
        QPointer<QTcpSocket> instSock;       // -> 实例 RTSP 端口
        QPointer<QUdpSocket> ntpSock;        // NTP 隧道本地 UDP socket
        quint16 ntpLocalPort = 0;            // 隧道绑定端口(= iPhone timingPort)
        int connectRetries = 0;
        QTimer recycleTimer;                 // 回收倒计时
    };

    explicit AirPlayGateway(const Config &cfg, SessionManager *mgr, QObject *parent = nullptr);
    ~AirPlayGateway() override;

    bool start();
    void stop();
    bool isRunning() const { return m_running; }

signals:
    void gatewayReady(const QString &deviceName, quint16 port);
    void clientConnected(const QString &clientIp, MirrorSession *session);
    void clientDisconnected(const QString &clientIp, MirrorSession *session);
    /** 来源手机信息就绪(名称/型号) */
    void clientInfoChanged(MirrorSession *session,
                           const QString &clientName, const QString &clientModel);
    /** 实例实际视频解码器就绪(avdec_h264 等, 用于软/硬解能力判断) */
    void decoderChanged(MirrorSession *session, const QString &decoder);
    void logMessage(const QString &message);

private:
    Instance *createInstance(const QString &clientIp);
    void destroyInstance(Instance *inst);
    void maybeStartMdns();
    void routeConnection(QTcpSocket *sock);
    void connectToInstance(Instance *inst);
    void retryConnectToInstance(Instance *inst);
    void flushClientToInstance(Instance *inst);
    void flushInstanceToClient(Instance *inst);
    void onSessionLog(Instance *inst, const QString &line);
    void onInstanceWindow(Instance *inst, qulonglong handle);
    void onClientSocketGone(Instance *inst);
    void onInstanceSocketGone(Instance *inst);
    void teardownLink(Instance *inst);
    void startRecycle(Instance *inst);
    void cancelRecycle(Instance *inst);
    void beginNtpTunnel(Instance *inst, quint16 timingPort);
    void stopNtpTunnel(Instance *inst);
    quint16 allocBase();
    void freeBase(quint16 base);
    bool instanceReady(const Instance *inst) const;
    QString findLocalIpv4() const;

    Config m_cfg;
    SessionManager *m_mgr = nullptr;
    QTcpServer *m_server = nullptr;
    MdnsBroadcaster *m_mdns = nullptr;
    QList<Instance *> m_instances;
    QHash<QString, Instance *> m_clientMap;   // clientIp -> instance
    QSet<quint16> m_freeBases;
    quint16 m_nextPort = 0;
    QString m_sharedPk;
    bool m_mdnsStarted = false;
    bool m_running = false;
    bool m_stopping = false;
};

} // namespace mirror
