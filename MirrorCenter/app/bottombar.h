#pragma once

#include <QWidget>
#include <QString>

class QToolButton;
class QButtonGroup;
class QHBoxLayout;

#include "systemstatus.h"
#include "volumecontrol.h"

/**
 * 底部控制栏
 * - 左侧:系统状态(SystemStatus)
 * - 中央:6 个布局选择按钮(胶囊容器)
 * - 右侧:音量控制
 */
class BottomControlBar : public QWidget
{
    Q_OBJECT
public:
    explicit BottomControlBar(QWidget *parent = nullptr);

    /** 设置/获取当前布局模式 1/2/3/4/6/Custom */
    void setLayoutMode(int mode);
    int  layoutMode() const { return m_currentMode; }

    SystemStatus  *systemStatus()  const { return m_status; }
    VolumeControl *volumeControl() const { return m_volume; }

signals:
    /** 布局变化(1/2/3/4/6/99) */
    void layoutModeChanged(int mode);

private:
    void buildLayoutPill();

    SystemStatus  *m_status       = nullptr;
    VolumeControl *m_volume       = nullptr;
    QWidget       *m_pill         = nullptr;
    QButtonGroup  *m_btnGroup     = nullptr;
    int            m_currentMode  = 1;
};
