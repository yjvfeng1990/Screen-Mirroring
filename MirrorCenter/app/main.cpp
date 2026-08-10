#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QMessageLogContext>
#include <QTimer>
#include <QScreen>
#include <QToolButton>
#include "desktopwindow.h"
#include "controlpanel.h"
#include "sidebar.h"
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

    // ============ 2) 悬浮控制台 ============
    ControlPanel panel;
    QList<Sidebar::NavGroup> groups;
    {
        Sidebar::NavGroup g;
        g.title = QStringLiteral("投屏控制");
        g.items = {
            { QStringLiteral("nav.layout"),     QStringLiteral("▦"), QStringLiteral("布局模式")   },
            { QStringLiteral("nav.screens"),    QStringLiteral("▤"), QStringLiteral("画面管理")   },
            { QStringLiteral("nav.scenes"),     QStringLiteral("◫"), QStringLiteral("场景管理")   },
            { QStringLiteral("nav.presets"),    QStringLiteral("▢"), QStringLiteral("预设布局")   },
        };
        groups.append(g);
    }
    {
        Sidebar::NavGroup g;
        g.title = QStringLiteral("信号源");
        g.items = {
            { QStringLiteral("nav.src.airplay"),  QStringLiteral("🍎"), QStringLiteral("AirPlay 接收")   },
            { QStringLiteral("nav.src.miracast"), QStringLiteral("📡"), QStringLiteral("Miracast 接收") },
            { QStringLiteral("nav.src.devices"),  QStringLiteral("📱"), QStringLiteral("投屏设备列表") },
            { QStringLiteral("nav.src.stream"),   QStringLiteral("🌐"), QStringLiteral("网络流媒体")   },
        };
        groups.append(g);
    }
    {
        Sidebar::NavGroup g;
        g.title = QStringLiteral("系统工具");
        g.items = {
            { QStringLiteral("nav.sys.audio"),    QStringLiteral("🔊"), QStringLiteral("音频控制")   },
            { QStringLiteral("nav.sys.settings"), QStringLiteral("⚙"),  QStringLiteral("系统设置")   },
            { QStringLiteral("nav.sys.network"),  QStringLiteral("📶"), QStringLiteral("网络设置")   },
            { QStringLiteral("nav.sys.devices"),  QStringLiteral("🖥"), QStringLiteral("设备管理")   },
            { QStringLiteral("nav.sys.log"),      QStringLiteral("📋"), QStringLiteral("日志中心")   },
        };
        groups.append(g);
    }
    panel.setNavGroups(groups);
    panel.selectNavKey("nav.layout");
    panel.setLayoutMode(1);

    // 默认位置:屏幕右侧居中
    {
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        const int x = screen.right() - panel.width() - 40;
        const int y = screen.center().y() - panel.height() / 2;
        panel.move(x, y);
    }
    panel.show();

    // ============ 3) 信号连接 ============
    // 布局模式变更
    QObject::connect(&panel, &ControlPanel::layoutModeChanged,
                     &desktop, &DesktopWindow::setLayoutMode);
    // 关闭按钮
    QObject::connect(&panel, &ControlPanel::hideRequested,
                     [&desktop]() { desktop.showToggleCtrlBtn(true); });
    // "显示控制台"小按钮
    QObject::connect(desktop.toggleCtrlButton(), &QToolButton::clicked,
                     [&panel, &desktop]() {
                         panel.setPanelVisible(true);
                         desktop.showToggleCtrlBtn(false);
                     });
    // 全屏切换
    QObject::connect(&panel, &ControlPanel::fullscreenToggleRequested,
                     &desktop, &DesktopWindow::toggleFullscreen);
    // 状态信息回显到面板标题栏(简单演示)
    QObject::connect(&desktop, &DesktopWindow::statusMessage,
                     [&panel](const QString &msg) {
                         qInfo() << "[status]" << msg;
                     });
    // 投屏帮助:暂时 = 重置布局到单屏
    QObject::connect(&panel, &ControlPanel::helpRequested,
                     [&panel, &desktop]() {
                         panel.setLayoutMode(1);
                     });
    // 录屏 / 录制 / 更多:目前仅日志
    QObject::connect(&panel, &ControlPanel::recordScreenRequested,
                     []() { qInfo() << "[ui] 录屏按钮点击(后续接入 ffmpeg)"; });
    QObject::connect(&panel, &ControlPanel::recordRequested,
                     []() { qInfo() << "[ui] 录制按钮点击(后续接入 SDK)"; });
    QObject::connect(&panel, &ControlPanel::moreRequested,
                     []() { qInfo() << "[ui] 更多按钮点击(后续接入菜单)"; });
    // 折叠底部
    QObject::connect(&panel, &ControlPanel::bottomCollapseToggled,
                     [](bool collapsed) { qInfo() << "[ui] 底部折叠:" << collapsed; });

    // 导航点击:信号源 → AirPlay / Miracast 启动对应会话
    QObject::connect(&panel, &ControlPanel::navItemClicked,
                     [&desktop](const QString &key) {
                         if (key == "nav.src.airplay") {
                             QTimer::singleShot(0, &desktop, &DesktopWindow::startAirPlay);
                         } else if (key == "nav.src.miracast") {
                             QTimer::singleShot(0, &desktop, &DesktopWindow::startMiracast);
                         } else {
                             qInfo() << "[ui] nav:" << key;
                         }
                     });

    // 启动后自动开启全部投屏服务
    QTimer::singleShot(200, &desktop, &DesktopWindow::startAirPlay);
    QTimer::singleShot(400, &desktop, &DesktopWindow::startMiracast);

    qInfo() << "entering event loop";
    return app.exec();
}
