#include "sessionmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

namespace mirror {

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
}

MirrorSession *SessionManager::createSession(BackendType type,
                                             const QString &deviceName,
                                             const QString &backendExe,
                                             const QStringList &backendArgs)
{
    // 单调递增计数生成 id:不能用 m_sessions.size()+1,
    // 中间会话被移除后 size 会变小, 导致新会话复用已存在会话的 id,
    // SDK 按 core->id() 分派窗口/状态回调时会匹配到错误的会话句柄,
    // 表现即第二个投屏窗口被挂到第一个设备的视图上, UI 嵌入时卡死。
    static quint64 s_idCounter = 0;
    const QString id = QStringLiteral("s%1").arg(++s_idCounter, 3, 10, QLatin1Char('0'));
    auto *session = new MirrorSession(id, type, deviceName, backendExe, backendArgs, this);
    m_sessions.insert(id, session);

    connect(session, &MirrorSession::stateChanged,
            this, [this, id](const QString &, mirror::SessionState s) { emit sessionStateChanged(id, s); });
    connect(session, &MirrorSession::windowReady,
            this, [this, id](const QString &, qulonglong h) { emit sessionWindowReady(id, h); });
    connect(session, &MirrorSession::frameReady,
            this, [this, id](const QString &) { emit sessionFrameReady(id); });
    connect(session, &MirrorSession::logMessage,
            this, [this, id](const QString &, const QString &m) { emit sessionLog(id, m); });
    connect(session, &QObject::destroyed, this, &SessionManager::onSessionDestroyed);

    emit sessionCreated(session);
    return session;
}

MirrorSession *SessionManager::session(const QString &id) const
{
    return m_sessions.value(id, nullptr);
}

QList<MirrorSession *> SessionManager::sessions() const
{
    return m_sessions.values();
}

QString SessionManager::findBackendExe(const QStringList &candidates) const
{
    for (const QString &c : candidates) {
        if (QDir::isAbsolutePath(c)) {
            if (QFile::exists(c))
                return c;
        } else {
            const QString base = QCoreApplication::applicationDirPath();
            const QString abs = QDir(base).filePath(c);
            if (QFile::exists(abs))
                return abs;
            if (QFile::exists(c))
                return c;
        }
    }
    return QString();
}

void SessionManager::stopAll()
{
    for (MirrorSession *s : m_sessions.values()) {
        s->stop();
    }
}

void SessionManager::onSessionDestroyed(QObject *obj)
{
    const auto it = std::find_if(m_sessions.cbegin(), m_sessions.cend(),
                                 [obj](MirrorSession *s) { return s == obj; });
    if (it != m_sessions.cend()) {
        const QString id = it.key();
        m_sessions.erase(it);
        emit sessionRemoved(id);
    }
}

} // namespace mirror
