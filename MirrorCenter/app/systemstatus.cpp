#include "systemstatus.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QDateTime>
#include <QRandomGenerator>
#include <QString>

SystemStatus::SystemStatus(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("systemStatusRoot");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 8);
    root->setSpacing(6);

    // -------- 指标行 --------
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(4);
    grid->setContentsMargins(0, 0, 0, 0);

    buildMetric(m_cpu,     QStringLiteral("CPU"),  QStringLiteral("%"));
    buildMetric(m_mem,     QStringLiteral("内存"),  QStringLiteral("%"));
    buildMetric(m_net,     QStringLiteral("网络"),  QStringLiteral("Mbps"));
    buildMetric(m_storage, QStringLiteral("存储"),  QStringLiteral("%"));

    grid->addWidget(m_cpu.nameLabel,     0, 0); grid->addWidget(m_cpu.bar,     1, 0, 1, 3);
    grid->addWidget(m_mem.nameLabel,     0, 3); grid->addWidget(m_mem.bar,     1, 3, 1, 3);
    grid->addWidget(m_net.nameLabel,     0, 6); grid->addWidget(m_net.bar,     1, 6, 1, 3);
    grid->addWidget(m_storage.nameLabel, 0, 9); grid->addWidget(m_storage.bar, 1, 9, 1, 3);

    // 把每行的 value/unit label 叠加到进度条上方右侧
    auto placeValue = [grid](Metric &m, int colStart) {
        grid->addWidget(m.valueLabel, 0, colStart + 1);
        grid->addWidget(m.unitLabel,  0, colStart + 2);
    };
    placeValue(m_cpu,     0);
    placeValue(m_mem,     3);
    placeValue(m_net,     6);
    placeValue(m_storage, 9);

    root->addLayout(grid);

    // -------- 时钟 --------
    m_clockLabel = new QLabel(this);
    m_clockLabel->setObjectName("systemClock");
    m_clockLabel->setText(QStringLiteral("--:--:--"));
    m_dateLabel = new QLabel(this);
    m_dateLabel->setObjectName("systemDate");
    m_dateLabel->setText(QStringLiteral("----/--/-- ------"));

    auto *timeRow = new QHBoxLayout();
    timeRow->setContentsMargins(0, 0, 0, 0);
    timeRow->setSpacing(8);
    timeRow->addWidget(m_clockLabel);
    timeRow->addWidget(m_dateLabel);
    timeRow->addStretch(1);
    root->addLayout(timeRow);

    // 定时刷新
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &SystemStatus::refresh);
    m_timer->start();
    refresh();
}

void SystemStatus::buildMetric(Metric &m, const QString &name, const QString &unit)
{
    m.unit = unit;

    m.nameLabel = new QLabel(name, this);
    m.nameLabel->setObjectName("statusLabel");
    m.nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m.valueLabel = new QLabel(QStringLiteral("0"), this);
    m.valueLabel->setObjectName("statusValue");
    m.valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m.unitLabel = new QLabel(unit, this);
    m.unitLabel->setObjectName("statusUnit");
    m.unitLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m.bar = new QProgressBar(this);
    m.bar->setObjectName("statusBar");
    m.bar->setRange(0, 100);
    m.bar->setValue(0);
    m.bar->setTextVisible(false);
    m.bar->setFixedHeight(3);
}

void SystemStatus::refresh()
{
    // 时钟
    const QDateTime now = QDateTime::currentDateTime();
    m_clockLabel->setText(now.toString(QStringLiteral("HH:mm:ss")));
    static const char *weekdays[] = {
        "星期日","星期一","星期二","星期三","星期四","星期五","星期六",
    };
    m_dateLabel->setText(now.toString(QStringLiteral("yyyy-MM-dd "))
                         + QString::fromUtf8(weekdays[now.date().dayOfWeek() % 7]));

    // 模拟数据(平滑抖动)
    auto *rng = QRandomGenerator::global();
    auto bump = [rng](int cur, int min, int max) {
        int step = rng->bounded(7) - 3;          // -3..+3
        int v = cur + step;
        if (v < min) v = min;
        if (v > max) v = max;
        return v;
    };
    m_cpu.fakeValue     = bump(m_cpu.fakeValue,     8, 75);
    m_mem.fakeValue     = bump(m_mem.fakeValue,    20, 85);
    m_storage.fakeValue = bump(m_storage.fakeValue, 30, 90);
    m_net.fakeValue     = bump(m_net.fakeValue,     2, 80);

    m_cpu.bar    ->setValue(m_cpu.fakeValue);
    m_mem.bar    ->setValue(m_mem.fakeValue);
    m_storage.bar->setValue(m_storage.fakeValue);
    m_net.bar    ->setValue(m_net.fakeValue);

    m_cpu.valueLabel    ->setText(QString::number(m_cpu.fakeValue));
    m_mem.valueLabel    ->setText(QString::number(m_mem.fakeValue));
    m_storage.valueLabel->setText(QString::number(m_storage.fakeValue));
    m_net.valueLabel    ->setText(QString::number(m_net.fakeValue));
}
