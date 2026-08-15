#include <QApplication>
#include <QSurfaceFormat>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QMessageLogContext>
#include <QTimer>
#include <QToolButton>
#include <QSharedMemory>
#include "desktopwindow.h"
#include "controlpanel.h"
#include "mirror_api.h"

#ifdef _WIN32
#include <windows.h>
#endif

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

    // ---- 单实例限制 ----
    // 用共享内存作互斥锁: 同一会话首次 create 成功, 二次启动 create 失败。
    // 防止两个实例各自拉起 Miracast/UWP 接收进程互相干扰(实测可致 GO 栈卡死)。
    QSharedMemory singleInst(QStringLiteral("MirrorCenter_SingleInstance_v2"));
    if (!singleInst.create(1)) {
        qWarning() << "Another MirrorCenter instance is running, exit.";
        // 激活已运行实例的主窗口(置顶到前台)
#ifdef _WIN32
        HWND h = FindWindowW(nullptr, L"MirrorCenter 投屏接收中心");
        if (h) {
            if (IsIconic(h))
                ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        }
#endif
        return 0;
    }

    // GPU 帧控件(QOpenGLWidget)默认表面格式:Desktop OpenGL 兼容 profile
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
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
    // 预创建 Miracast 占位视图(4 路槽位): 必须在窗口显示前完成。
    // QOpenGLWidget 在已显示窗口上动态创建会触发父窗口 HWND 重建
    // (Qt6 行为), 表现为"窗口打开后又消失重显"。窗口未显示前创建则无此问题。
    desktop.createMiracastPlaceholders();
    // createWinId(): 在 show 之前强制创建原生窗口(HWND)。
    // 否则 show() 时才创建原生窗口, 创建后 QSS/布局首次应用会触发
    // Qt 销毁并重建 HWND → 视觉上"关了一下又打开"。提前创建后
    // 样式与布局都在离屏状态下完成, show() 一次性呈现, 无重建闪烁。
    desktop.createWinId();
#ifdef _WIN32
    // 消除首次 ShowWindow 的系统背景白闪: 窗口类擦背景画刷默认是浅色
    // (COLOR_WINDOW), 对纯黑主窗口表现为"闪一下白"。改为黑色画刷后,
    // 窗口显示前背景即为黑, 无白色闪现(含从任务栏还原等每次显示)。
    {
        HWND hwnd = reinterpret_cast<HWND>(desktop.winId());
        ::SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                           reinterpret_cast<LONG_PTR>(::GetStockObject(BLACK_BRUSH)));
    }
#endif
    // 首帧闪屏消除: 窗口首次显示时若 QSS/布局尚未应用到子控件, 会先以系统
    // 默认浅色渲染一帧(灰白闪), 随后才变深色主题。这里在屏幕外先完成
    // HWND 创建 + 样式应用 + 布局激活 + 首帧绘制(用户不可见), 再移回
    // 屏幕正常显示 —— 用户看到窗口时首帧已是最终深色画面, 无白屏。
    // 注意: 全程不调用 hide(), 只移动窗口位置 —— 否则用户会看到
    // "窗口出现 → 消失 → 再出现"。
    desktop.ensurePolished();
    {
        const QPoint savedPos = desktop.pos();
        desktop.move(QPoint(-100000, -100000));   // 移到屏幕外预渲染
        desktop.show();                            // 屏幕外完成 HWND 创建与首帧绘制
        QApplication::processEvents();             // 完成布局/样式首轮应用
        desktop.repaint();                         // 强制完成首帧绘制
        desktop.move(savedPos);                    // 移回原始位置(窗口始终可见, 无消失)
    }

    // ============ 2) 侧边"投屏来源"面板(内嵌主窗口) ============
    // 面板作为主窗口子控件: 展开时覆盖主窗口右侧内部, 收起完全隐藏,
    // 随主窗口最小化/移动/关闭一起走, 不再悬浮桌面。
    ControlPanel panel(&desktop);
    panel.setPanelVisible(false);

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
    // 主窗口右缘触发条(内嵌) → 展开面板
    QObject::connect(&desktop, &DesktopWindow::sideTriggerActivated,
                     [&panel, &desktop]() {
                         panel.setPanelVisible(true);
                         desktop.showToggleCtrlBtn(false);
                     });
    // 面板展开/收起 → 隐藏/恢复右缘触发条(独立顶层窗会浮在面板之上)
    QObject::connect(&panel, &ControlPanel::panelVisibilityChanged,
                     [&desktop](bool panelVisible) {
                         desktop.setSideTriggerVisible(!panelVisible);
                     });
    // 主窗口关闭 → 联动关闭面板(否则面板残留, app 不退出)
    QObject::connect(&desktop, &DesktopWindow::closeRequested,
                     [&panel]() { panel.close(); });
    // 主窗口最小化/还原 → 联动隐藏/还原面板与触发条
    QObject::connect(&desktop, &DesktopWindow::windowMinimizedChanged,
                     &panel, &ControlPanel::setHostMinimized);
    QObject::connect(&desktop, &DesktopWindow::windowMinimizedChanged,
                     [&desktop](bool minimized) {
                         desktop.setSideTriggerVisible(!minimized);
                     });
    // 面板"移除投屏设备"按钮 → 断开该设备会话(网关先断连再清理视图)
    QObject::connect(&panel, &ControlPanel::removeRequested,
                     &desktop, &DesktopWindow::removeSession);
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
