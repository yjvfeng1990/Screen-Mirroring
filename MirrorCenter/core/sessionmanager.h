#pragma once

#include "mirrorsession.h"

#include <QObject>
#include <QHash>

namespace mirror {

/**
 * 会话管理器(无 UI,core 库核心)。
 * 创建/销毁 MirrorSession,维护会话表,负责解析命令行参数中的后端可执行文件路径。
 */
class SessionManager : public QObject
{
    Q_OBJECT
public:
    explicit SessionManager(QObject *parent = nullptr);

    MirrorSession *createSession(BackendType type,
                                 const QString &deviceName,
                                 const QString &backendExe,
                                 const QStringList &backendArgs = {});
    MirrorSession *session(const QString &id) const;
    QList<MirrorSession *> sessions() const;
    int count() const { return m_sessions.size(); }

    /// 查找后端可执行文件:依次尝试候选路径(绝对路径或相对应用目录)
    QString findBackendExe(const QStringList &candidates) const;

    void stopAll();

signals:
    void sessionCreated(MirrorSession *session);
    void sessionRemoved(const QString &id);
    void sessionStateChanged(const QString &id, mirror::SessionState state);
    void sessionWindowReady(const QString &id, qulonglong handle);
    void sessionFrameReady(const QString &id);
    void sessionLog(const QString &id, const QString &message);
    /// 投屏设备真实名称(服务端经帧通道上报)
    void sessionClientInfo(const QString &id, const QString &name, const QString &model);

private slots:
    void onSessionDestroyed(QObject *obj);

private:
    QHash<QString, MirrorSession *> m_sessions;
};

} // namespace mirror
