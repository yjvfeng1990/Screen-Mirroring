#pragma once

#include <QWidget>
#include <QStringList>

class QComboBox;
class QToolButton;
class QLabel;

/**
 * 顶部栏(EdgeCast Studio 风格)
 * - 居中:布局选择器(胶囊下拉)
 * - 右侧:全屏按钮
 */
class TopBar : public QWidget
{
    Q_OBJECT
public:
    explicit TopBar(QWidget *parent = nullptr);

    /** 设置顶部布局选项(空表示不修改) */
    void setLayoutOptions(const QStringList &options, int currentIndex = 0);

    /** 设置当前布局显示文本 */
    void setCurrentLayoutText(const QString &text);

signals:
    /** 顶部布局选择变化 */
    void layoutOptionChanged(const QString &text);
    /** 全屏 */
    void fullscreenClicked();

private:
    QComboBox   *m_layoutCombo = nullptr;
    QToolButton *m_btnFullscreen = nullptr;
};
