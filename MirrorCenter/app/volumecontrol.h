#pragma once

#include <QWidget>

class QSlider;
class QLabel;
class QToolButton;

/**
 * 音量控制(底部右侧)
 * 扬声器图标 + 横向滑条 + 百分比 + 设置图标
 */
class VolumeControl : public QWidget
{
    Q_OBJECT
public:
    explicit VolumeControl(QWidget *parent = nullptr);
    int value() const;
    void setValue(int v);

signals:
    void valueChanged(int v);
    void settingsClicked();
    void muteToggled(bool muted);

private:
    QToolButton *m_iconBtn   = nullptr;
    QSlider     *m_slider    = nullptr;
    QLabel      *m_percent   = nullptr;
    QToolButton *m_settings  = nullptr;
    bool         m_muted     = false;
    int          m_lastValue = 60;
};
