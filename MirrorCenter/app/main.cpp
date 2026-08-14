#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QMessageLogContext>
#include <QTimer>
#include <QToolButton>
#include "desktopwindow.h"
#include "controlpanel.h"
#include "mirror_api.h"

static void logToFile(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    QString dir = QDir::homePath() + QStringLiteral("/AppData/Local/MirrorCenter");
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/mirrorcenter.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))
           << QStringLiteral(" [%1] ").arg(int(type)) << msg << QChar(u'\n');
    }
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(logToFile);
    qInfo() << "=== MirrorCenter start ===";
    QApplication app(argc, argv);
    qInfo() << "QApplication created";
    app.setApplicationName("MirrorCenter");
    app.setOrganizationName("ScreenMirror");

    QFile qss(QStringLiteral(":/theme.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
        qInfo() << "theme.qss applied";
    } else {
        qWarning() << "theme.qss open failed";
    }

    // 初始化 SDK
    mirror_init();

    // ============ 1) 桌面窗口(主窗口) ============
    DesktopWindow desktop;
    desktop.show();

    // ============ 2) 侧边吸附"投屏来源"面板 ============
    ControlPanel panel;
    // 面板吸附在屏幕右边缘, 由面板内部定位; 初始展开显示
    panel.show();

    // ============ 3) 信号连接 ============
    // 来源列表刷新:桌面会话增删/设备名就绪 → 拉取列表 → 更新面板
    auto refreshSources = [&panel, &desktop]() {
        QList<ControlPanel::SourceInfo> items;
        for (const SourceItem &it : desktop.sourceItems()) {
            ControlPanel::SourceInfo si;
            si.sessionId = it.sessionId;
            si.name   = it.name;
            si.ip     = it.ip;
            si.status = it.status;
            items.append(si);
        }
        panel.setSources(items);
    };
    QObject::connect(&desktop, &DesktopWindow::sourcesChanged,
                     &desktop, refreshSources);
    refreshSources();   // 初始刷新(此时无设备, 显示空状态)

    // 缩略图 provider:控制台定时向桌面窗口拉取各会话画面
    panel.setThumbnailProvider([&desktop](const QString &sessionId) {
        return desktop.thumbnailFor(sessionId);
    });

    // 列表点击选中 → 该来源窗口在主窗口首位
    QObject::connect(&panel, &ControlPanel::sourceSelected,
                     &desktop, &DesktopWindow::focusSession);

    // "×" 收起面板 → 桌面右下角显示"显示控制台"小按钮
    QObject::connect(&panel, &ControlPanel::hideRequested,
                     [&desktop]() { desktop.showToggleCtrlBtn(true); });
    // "显示控制台"小按钮 → 重新展开面板
    QObject::connect(desktop.toggleCtrlButton(), &QToolButton::clicked,
                     [&panel, &desktop]() {
                         panel.setPanelVisible(true);
                         desktop.showToggleCtrlBtn(false);
                     });
    // 状态信息回显(调试用)
    QObject::connect(&desktop, &DesktopWindow::statusMessage,
                     [](const QString &msg) { qInfo() << "[status]" << msg; });

    // 启动后自动开启 AirPlay 网关(多设备自动调度);
    QTimer::singleShot(200, &desktop, &DesktopWindow::startAirPlay);
    // 启动后自动开启 Miracast 接收(激活 UWP 接收进程, 安卓可搜到);
    QTimer::singleShot(600, &desktop, &DesktopWindow::startMiracast);
    // 启动后自动开启 MS-MICE 接收端(Win+K 基础设施投屏, Windows 笔记本可搜到);
    QTimer::singleShot(1000, &desktop, &DesktopWindow::startMiceBackend);

    qInfo() << "entering event loop";
    return app.exec();
}
