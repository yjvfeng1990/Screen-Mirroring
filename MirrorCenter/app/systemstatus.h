#pragma once

#include <QWidget>

class QVBoxLayout;
class QLabel;
class QProgressBar;
class QTimer;

/**
 * 系统状态面板(底部左侧)
 * - CPU / 内存 / 网络 / 存储 四项,每项 label + 进度条 + 百分比
 * - 下方时间(时:分:秒 + 日期)
 *
 * 模拟数据(本地读不到全局性能时,用 0~100 抖动数据,后续可接入真实系统接口)
 */
class SystemStatus : public QWidget
{
    Q_OBJECT
public:
    explicit SystemStatus(QWidget *parent = nullptr);

private slots:
    void refresh();

private:
    struct Metric {
        QLabel     *nameLabel   = nullptr;
        QLabel     *valueLabel  = nullptr;
        QLabel     *unitLabel   = nullptr;
        QProgressBar *bar       = nullptr;
        QString     unit;       // 内存/网络/存储单位
        int         fakeValue   = 0;
    };
    void buildMetric(Metric &m, const QString &name, const QString &unit);

    QLabel    *m_clockLabel = nullptr;
    QLabel    *m_dateLabel  = nullptr;
    QTimer    *m_timer      = nullptr;

    Metric m_cpu;
    Metric m_mem;
    Metric m_net;
    Metric m_storage;
};
