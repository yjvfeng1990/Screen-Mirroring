#pragma once

#include <QWidget>
#include <QPointer>
#include <QImage>
#include <QTimer>
#include <QPixmap>
#include <QPainter>
#include <QElapsedTimer>
#include <QOpenGLWidget>

class QLabel;
class QWindow;
class QOpenGLTexture;
class QOpenGLShaderProgram;

/**
 * 帧显示控件:QOpenGLWidget + GPU 纹理缩放。
 * 替代自绘 QWidget 的软件路径 —— 软缩放(CPU) + QPixmap::fromImage(CPU 拷贝)
 * + 软件合成(CPU) 全部改由 GPU 完成:帧零拷贝包装后 setFrame 上传为纹理,
 * paintGL 中 QPainter::drawTexture 用 GPU 完成缩放/裁切/留边, CPU 仅算矩形。
 * 每会话独立 GL 控件/纹理(多路分屏互不影响)。
 */
class GLFrameSurface : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit GLFrameSurface(QWidget *parent = nullptr);
    ~GLFrameSurface() override;

    /** 上传帧并标记重绘:src 为源图像像素区域(黑边裁切), dst 为目标区域(高度优先/等比留边)。
     *  上传同步完成, 调用后 img 可安全释放(SDK 帧 buffer 无需持有)。 */
    void setFrame(const QImage &img, const QRectF &src, const QRectF &dst);
    void clearFrame();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    QRectF m_src;                 // 源图像采样区域(像素)
    QRectF m_dst;                 // 目标绘制区域(控件坐标)
    QOpenGLTexture *m_tex = nullptr;
    QOpenGLShaderProgram *m_prog = nullptr;
};

// 引入 SDK C 接口
#include "mirror_api.h"

/**
 * 单个投屏会话视图(SDK 演示宿主)。
 * Miracast:通过 SDK 启动 UWP 接收进程 → 帧回调中取帧 → 绘制到 QLabel(自建会话)。
 * AirPlay 网关:视图由网关回调提供的 SDK 句柄包装而来(adoptGatewaySession),
 *            不拥有句柄生命周期, 设备断开时句柄由 SDK 回收, 视图仅解除嵌入。
 */
class SessionView : public QWidget
{
    Q_OBJECT
public:
    explicit SessionView(const QString &deviceName,
                         mirror_backend_t backend,
                         QWidget *parent = nullptr,
                         bool deferStart = false);
    ~SessionView() override;

    QString deviceName() const { return m_deviceName; }
    QString sessionId() const { return m_sessionId; }
    QString clientIp() const { return m_clientIp; }
    bool isRunning() const { return m_running; }
    mirror_backend_t backend() const { return m_backend; }
    bool isGatewayMode() const { return m_gatewayMode; }
    mirror_session_t *sdkSession() const { return m_sdkSession; }

    /** 信息栏相对本视图重定位(主窗口移动/跨屏时由 DesktopWindow 调用) */
    void refreshInfoBadge() { updateInfoBadge(); }

    /** 网关模式:包装 SDK 已建好的会话句柄(由 on_client_connected 回调提供) */
    void adoptGatewaySession(mirror_session_t *sdkSession, const QString &clientIp);
    /** 手动会话:接管 SDK 已创建好的会话句柄(多路 Miracast 组用, 视图拥有句柄) */
    void adoptManualSession(mirror_session_t *sdkSession, const QString &deviceName);

    void stop();
    /** 设备断开:移除窗口嵌入(视图随后由 DesktopWindow 删除) */
    void detach();
    /** 主动清空画面回占位等待(移除投屏源/帧链路断开时用)。
     *  不依赖服务端断链回调时序 —— 宿主发起移除后立即调用, 画面即刻消失。 */
    void resetToWaiting();
    /** 由 DesktopWindow 通知当前是否独占全屏(切换按钮图标) */
    void setFullscreenActive(bool active);
    /** 全屏联动静音: 焦点路 false(出声), 其余路 true(静音)。
     *  Miracast 按连接 SETMUTE, AirPlay/MICE 按进程 PID。 */
    void setMuted(bool mute);
    /** 当前是否处于静音状态 */
    bool isMuted() const { return m_muted; }
    /** 由 DesktopWindow 通知当前是否铺满整格(≥2 路分屏:覆盖裁剪, 无黑边);
     *  false = 独立等比显示完整画面(留边)。 */
    void setFillMode(bool on);
    /** 更新来源手机名称/型号(显示在悬浮标签) */
    void setClientInfo(const QString &name, const QString &model);
    /** 当前画面缩略图(控制台列表用);无画面返回空 */
    QPixmap thumbnail() const;
    /** 是否有真实投屏内容:网关模式(AirPlay 已连接) / Miracast 已收到首帧 */
    bool isActive() const;

signals:
    void sessionClosed(const QString &sessionId);
    void statusChanged(const QString &sessionId, const QString &status);
    /** 用户点击了全屏/还原按钮(DesktopWindow 负责切换该会话独占全屏) */
    void fullscreenRequested(const QString &sessionId);
    /** 缩略图刷新(控制台列表据此更新) */
    void thumbnailUpdated();
    /** 收到首帧(自建会话真正出画, 此时才允许主窗口/列表显示) */
    void firstFrameReceived();
    /** 帧链路断开(设备退出):画面已清空, 回到等待状态(通知主窗口重排/刷新列表) */
    void firstFrameCleared();
    /** 设备真实名称已上报(服务端经帧通道 MCCTRL1 NAME: 送达) */
    void clientNameChanged(const QString &name);

protected:
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void buildUi();
    /** 自建会话(Miracast / 独立 AirPlay):通过 mirror_start_session 启动后端 */
    void startStandaloneSession();
    /** 把本视图回调挂到 SDK 会话上 */
    void attachCallbacks();
    void attachWindow(qulonglong wid);
    void setStatus(const QString &s);
    void renderFrame();
    /** 周期查询 Wifi 网卡性能计数器, 差分显示源网络实时接收/发送速率 */
    void queryNetRate();
    /** AirPlay 帧率/分辨率估算:窗口内容变化检测 + 客户区尺寸(无帧回调时用) */
    void estimateAirStats();
    /** 抓取当前画面(平台相关实现) */
    QImage captureThumbnail();
    /** 画面内容指纹比较:有变化返回 true(缩略图去重, 无变化不切图) */
    bool thumbChanged(const QImage &thumb);
    /** 信息标签为独立顶层小窗, 跟随本视图左上角悬浮(避免被 D3D11 原生窗口遮挡) */
    void updateInfoBadge();
    /** 静音/取消静音(按会话后端子进程 PID 控制音频会话) */
    void toggleMute();
    /** 执行静音/取消静音: Miracast 按连接 SETMUTE, 其它按进程 PID。返回是否成功。 */
    bool applyMute(bool mute);
    /** 全屏/还原 */
    void toggleFullscreen();

    // SDK 回调(C 函数指针)
    static void onStateCallback(mirror_session_t *session, mirror_state_t state, void *userdata);
    static void onWindowCallback(mirror_session_t *session, uint64_t handle, void *userdata);
    static void onLogCallback(mirror_session_t *session, const char *message, void *userdata);
    static void onFrameCallback(mirror_session_t *session, void *userdata);
    static void onClientInfoCallback(mirror_session_t *session, const char *name,
                                     const char *model, void *userdata);

    QString m_sessionId;
    QString m_deviceName;
    QString m_clientIp;              // 网关模式:已连接设备 IP
    mirror_backend_t m_backend;
    mirror_session_t *m_sdkSession = nullptr;
    bool m_gatewayMode = false;      // 句柄由网关提供, stop() 不销毁

    QPointer<QWindow> m_childWindow;
    qulonglong m_embeddedHwnd = 0;       // 已嵌入的原生窗口句柄(去重 + 重挂防抖)
    QWidget *m_videoArea      = nullptr;   // 视频区(占位/嵌入容器/Miracast 帧)
    QWidget *m_infoBadge      = nullptr;   // 悬浮信息标签(独立顶层窗, 设备名 + 状态)
    QLabel  *m_statusDot      = nullptr;   // 状态指示点
    QLabel  *m_statusLabel    = nullptr;   // 状态文字
    QLabel  *m_resLabel       = nullptr;   // 分辨率(仅一处显示, 避免与状态重复)
    QLabel  *m_fpsLabel       = nullptr;   // 帧率
    QLabel  *m_rateLabel      = nullptr;   // 上下行数据传输率
    QLabel  *m_devLabel       = nullptr;   // 来源手机名称(初始为 IP)
    GLFrameSurface *m_videoLabel = nullptr;  // Miracast 帧显示(GPU 纹理缩放, 无软件缩放 CPU 开销)
    class QToolButton *m_muteBtn = nullptr; // 静音切换
    class QToolButton *m_fullBtn = nullptr; // 全屏切换
    bool m_muted            = false;
    bool m_running          = false;
    QTimer m_thumbTimer;                 // 缩略图抓取节拍(内容变化时 1.2s, 静止时 3s)
    QPixmap m_lastThumb;                 // 最近一帧缩略图
    QImage m_thumbFp;                    // 上次画面指纹(16x9, 用于变化检测)
    bool m_hasThumbFp = false;
    // 帧率/分辨率统计(1s 滑动窗口, 在 renderFrame 累加)
    int m_rateFrames = 0;                // 窗口内累计帧数
    QElapsedTimer m_rateTimer;           // 窗口计时器
    int m_lastFps = 0;                   // 最近 1s 统计的帧率
    int m_lastW = 0, m_lastH = 0;        // 最近帧分辨率
    // 无线链路速率(GetIfTable2 全接口字节增量差分, 无外部进程开销)
    QTimer m_netTimer;                   // 周期查询定时器
    quint64 m_netPrevRx = 0;             // 上次采样全接口累计接收字节
    quint64 m_netPrevTx = 0;             // 上次采样全接口累计发送字节
    qint64  m_netPrevMs = 0;             // 上次采样时刻(ms, 0=首采样未就绪)
    // AirPlay 帧率估算(无帧回调, 用窗口内容变化检测):
    QTimer m_airStatsTimer;              // 估算节拍(1s)
    QImage m_airPrevFp;                  // 上次内容指纹(16x9)
    bool   m_hasAirPrevFp = false;
    int    m_airChanged = 0;             // 本窗口内检测到内容变化的采样次数
    int    m_airSamples  = 0;            // 本窗口内采样总数
    QElapsedTimer m_airWin;              // 1s 滑动窗口计时
    bool m_framePending = false;         // 上一帧是否还没在 UI 线程渲染完(节流用)
    bool m_hasFirstFrame = false;        // 是否已收到首帧(Miracast 占位会话据此隐藏)
    bool m_fillMode = false;             // 铺满整格:等比放大覆盖后居中裁剪(无黑边)
};
