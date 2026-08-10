#include "volumecontrol.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QToolButton>

VolumeControl::VolumeControl(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("volumeRoot");
    setFixedHeight(36);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(10, 0, 8, 0);
    root->setSpacing(8);

    m_iconBtn = new QToolButton(this);
    m_iconBtn->setObjectName("volumeIcon");
    m_iconBtn->setText(QStringLiteral("🔊"));
    m_iconBtn->setCursor(Qt::PointingHandCursor);
    m_iconBtn->setToolTip(QStringLiteral("静音/取消静音"));
    root->addWidget(m_iconBtn);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setObjectName("volumeSlider");
    m_slider->setRange(0, 100);
    m_slider->setValue(60);
    m_slider->setMinimumWidth(120);
    m_slider->setMaximumWidth(180);
    root->addWidget(m_slider, 1);

    m_percent = new QLabel(QStringLiteral("60%"), this);
    m_percent->setObjectName("volumePercent");
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_percent->setMinimumWidth(36);
    root->addWidget(m_percent);

    m_settings = new QToolButton(this);
    m_settings->setObjectName("volumeSettings");
    m_settings->setText(QStringLiteral("⚙"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setToolTip(QStringLiteral("音频设置"));
    root->addWidget(m_settings);

    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        m_percent->setText(QString::number(v) + QStringLiteral("%"));
        m_lastValue = v;
        if (v > 0 && m_muted) {
            m_muted = false;
            m_iconBtn->setText(QStringLiteral("🔊"));
        }
        emit valueChanged(v);
    });
    connect(m_iconBtn, &QToolButton::clicked, this, [this]() {
        m_muted = !m_muted;
        if (m_muted) {
            m_iconBtn->setText(QStringLiteral("🔇"));
            m_slider->setValue(0);
        } else {
            m_iconBtn->setText(QStringLiteral("🔊"));
            m_slider->setValue(m_lastValue > 0 ? m_lastValue : 60);
        }
        emit muteToggled(m_muted);
    });
    connect(m_settings, &QToolButton::clicked, this, &VolumeControl::settingsClicked);
}

int VolumeControl::value() const { return m_slider->value(); }

void VolumeControl::setValue(int v)
{
    m_slider->setValue(v);
}
