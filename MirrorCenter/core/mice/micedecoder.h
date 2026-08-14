#pragma once

#include <QObject>
#include <QImage>
#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>

class QThread;

namespace mirror {

/**
 * 基于 Windows Media Foundation(MFT)的 H.264 软件解码器。
 *
 * - 独立解码线程消费 Annex B 帧队列,避免阻塞 RTP 接收(主线程)
 * - 输入: 完整 H.264 帧(Annex B, 可为 SPS+PPS+IDR 多 NALU)
 * - 输出: BGRA8 QImage(与项目帧协议一致), 经 frameReady 信号跨线程投递
 * - 解码慢时自动丢旧帧, 保证实时性
 */
class MiceDecoder : public QObject
{
    Q_OBJECT
public:
    explicit MiceDecoder(QObject *parent = nullptr);
    ~MiceDecoder() override;

    bool start();
    void stop();

    /** 线程安全: 投递一帧 Annex B 到解码队列 */
    void pushFrame(const QByteArray &annexB);

signals:
    void frameReady(const QImage &frame);
    void error(const QString &why);

public:
    void decodeLoop();

    QThread *m_thread = nullptr;
    QMutex m_mutex;
    QWaitCondition m_cond;
    QQueue<QByteArray> m_queue;
    bool m_stopping = false;
};

} // namespace mirror
