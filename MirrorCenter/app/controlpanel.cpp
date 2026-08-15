#include "controlpanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QToolButton>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QTimer>
#include <QApplication>
#include <QScreen>
#include <QCursor>
#include <QPixmap>
#include <QStyle>
#include <QGraphicsDropShadowEffect>
#include <algorithm>

ControlPanel::ControlPanel(QWidget *parent)
    : QWidget(parent)
{
    // 独立顶层 Tool 窗(带 parent 跟随主窗口), 置顶防被 AirPlay 嵌入的 D3D11
    // 渲染窗口(uxplay swap chain)盖住 —— 子控件 raise() 压不住翻转模型渲染
    // 窗口, 与信息栏 infoBadge 同款方案(Qt::Tool + WindowStaysOnTopHint)。
    // 收起 = hide() 完全隐藏, 展开 = show()+raise() 覆盖在主窗口右侧, 跟随
    // 主窗口最小化/移动(顶层子窗自动跟随 + Move 事件兜底重定位)。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);  // 外层透明,圆角由 m_rootFrame 实现
    setMinimumSize(300, 400);
    resize(320, 600);

    buildUi();

    // 阴影
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 160));
    m_rootFrame->setGraphicsEffect(shadow);

    // ---- 内嵌面板 ----
    m_panelWidth = width();

    // 鼠标离开面板 300ms 后收起(面板内嵌主窗口, 移出面板即视为"点空白收回")
    m_hideTimer = new QTimer(this);
    m_hideTimer->setInterval(300);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, [this]() {
        if (!m_dockedExpanded)
            return;
        const QPoint pos = QCursor::pos();
        if (rect().contains(mapFromGlobal(pos)))
            return;
        collapse();
    });

    // 缩略图刷新节拍(仅展开时工作)
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(800);
    connect(m_thumbTimer, &QTimer::timeout, this, [this]() {
        if (m_dockedExpanded && isVisible())
            refreshThumbnails();
    });
    m_thumbTimer->start();

    // 初始: 收起(面板隐藏), 由触发条/按钮展开
    m_dockedExpanded = false;
    hide();

    // 内嵌面板: 监听父窗口 resize 跟随重定位; 点击父窗口空白处收回面板
    if (parentWidget())
        parentWidget()->installEventFilter(this);
}

ControlPanel::~ControlPanel() = default;

void ControlPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);  // 给阴影留空间
    outer->setSpacing(0);

    // 外层圆角深色面板
    m_rootFrame = new QFrame(this);
    m_rootFrame->setObjectName("ctrlPanelRoot");
    auto *rfL = new QVBoxLayout(m_rootFrame);
    rfL->setContentsMargins(0, 0, 0, 0);
    rfL->setSpacing(0);

    // 标题栏
    buildTitleBar();
    rfL->addWidget(m_titleBar);

    // 来源列表区
    m_listHost = new QWidget(m_rootFrame);
    m_listHost->setObjectName("ctrlPanelContent");
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(10, 10, 10, 10);
    m_listLayout->setSpacing(8);

    // 空状态
    m_emptyLabel = new QLabel(QStringLiteral("暂无投屏设备\n投屏后显示播放器窗口列表"), m_listHost);
    m_emptyLabel->setObjectName("ctrlPanelEmpty");
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_listLayout->addWidget(m_emptyLabel);
    m_listLayout->addStretch(1);

    rfL->addWidget(m_listHost, 1);

    outer->addWidget(m_rootFrame);
}

void ControlPanel::buildTitleBar()
{
    m_titleBar = new QFrame();
    m_titleBar->setObjectName("ctrlPanelTitleBar");
    m_titleBar->setFixedHeight(36);
    auto *tl = new QHBoxLayout(m_titleBar);
    tl->setContentsMargins(14, 0, 6, 0);
    tl->setSpacing(6);

    auto *ico = new QLabel(QStringLiteral("🖥️"));
    ico->setStyleSheet("background-color:transparent; font-size:14px;");
    tl->addWidget(ico);

    m_titleLabel = new QLabel(QStringLiteral("控制面板"));
    m_titleLabel->setStyleSheet(
        "color:#C2C9D6; font-size:13px; font-weight:600;"
        "background-color:transparent;");
    tl->addWidget(m_titleLabel);
    tl->addStretch(1);

    m_btnClose = new QToolButton();
    m_btnClose->setObjectName("ctrlPanelTitleBtn");
    m_btnClose->setText(QStringLiteral("×"));
    m_btnClose->setToolTip(QStringLiteral("收起到屏幕右边缘(悬停触发条可再次展开)"));
    m_btnClose->setFixedSize(24, 24);
    m_btnClose->setCursor(Qt::PointingHandCursor);
    connect(m_btnClose, &QToolButton::clicked, this, [this]() {
        collapse();
        emit hideRequested();
    });
    tl->addWidget(m_btnClose);
}

void ControlPanel::setThumbnailProvider(std::function<QPixmap(const QString &)> provider)
{
    m_thumbProvider = std::move(provider);
}

void ControlPanel::setSources(const QList<SourceInfo> &sources)
{
    // 清空旧卡片
    for (auto it = m_thumbLabels.begin(); it != m_thumbLabels.end(); ) {
        QLabel *lbl = it.value();
        if (lbl) {
            QWidget *card = lbl->parentWidget();
            if (card) {
                card->deleteLater();
                m_listLayout->removeWidget(card);
            }
        }
        it = m_thumbLabels.erase(it);
    }
    // 清掉可能残留的非空标签项(保险)
    while (m_listLayout->count() > 2) {
        QLayoutItem *item = m_listLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (sources.isEmpty()) {
        m_emptyLabel->show();
        updateSelection();
        return;
    }
    m_emptyLabel->hide();

    // 选中项保持;若已断开则清除
    if (m_selectedId.isEmpty() || !std::any_of(sources.begin(), sources.end(),
            [this](const SourceInfo &s) { return s.sessionId == m_selectedId; })) {
        m_selectedId.clear();
    }

    for (const SourceInfo &s : sources) {
        auto *card = new QFrame(m_listHost);
        card->setObjectName("srcItemCard");
        card->setProperty("sessionId", s.sessionId);
        card->setCursor(Qt::PointingHandCursor);
        // 点击卡片 → 选中置顶
        card->installEventFilter(this);
        card->setToolTip(QStringLiteral("点击将 %1 窗口置于主窗口首位").arg(s.name));

        auto *hl = new QHBoxLayout(card);
        hl->setContentsMargins(10, 8, 10, 8);
        hl->setSpacing(10);

        // 缩略图
        auto *thumb = new QLabel(card);
        thumb->setObjectName("srcItemThumb");
        thumb->setFixedSize(96, 54);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(
            "background-color:#0B0F1A; border-radius:6px;"
            "border:1px solid rgba(255,255,255,10); color:#6B7488;"
            "font-size:10px;");
        thumb->setText(QStringLiteral("等待画面"));
        hl->addWidget(thumb, 0, Qt::AlignVCenter);

        // 名称(仅显示来源名称)
        auto *name = new QLabel(s.name, card);
        name->setObjectName("srcItemName");
        name->setStyleSheet(
            "color:#E8ECF4; font-size:14px; font-weight:600;"
            "background-color:transparent;");
        name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        hl->addWidget(name, 1);

        // 移除按钮(断开该投屏设备)
        auto *btnRemove = new QToolButton(card);
        btnRemove->setObjectName("srcItemRemoveBtn");
        btnRemove->setText(QStringLiteral("✕"));
        btnRemove->setToolTip(QStringLiteral("移除该投屏设备"));
        btnRemove->setFixedSize(22, 22);
        btnRemove->setCursor(Qt::PointingHandCursor);
        connect(btnRemove, &QToolButton::clicked, this, [this, id = s.sessionId]() {
            emit removeRequested(id);
        });
        hl->addWidget(btnRemove, 0, Qt::AlignVCenter);

        m_listLayout->insertWidget(m_listLayout->count() - 2, card);

        m_thumbLabels.insert(s.sessionId, thumb);
    }
    updateSelection();
    refreshThumbnails();
}

void ControlPanel::refreshThumbnails()
{
    if (!m_thumbProvider)
        return;
    for (auto it = m_thumbLabels.begin(); it != m_thumbLabels.end(); ++it) {
        QLabel *lbl = it.value();
        if (!lbl)
            continue;
        const QPixmap p = m_thumbProvider(it.key());
        if (p.isNull())
            continue;
        // 画面无变化(cacheKey 相同)时跳过 setPixmap, 避免无谓重绘
        const QPixmap cur = lbl->pixmap();
        if (!cur.isNull() && cur.cacheKey() == p.cacheKey())
            continue;
        lbl->setText(QString());
        lbl->setPixmap(p.scaled(lbl->size(),
                                Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void ControlPanel::onItemClicked(const QString &sessionId)
{
    m_selectedId = sessionId;
    updateSelection();
    emit sourceSelected(sessionId);
}

void ControlPanel::updateSelection()
{
    for (int i = 0; i < m_listLayout->count(); ++i) {
        QLayoutItem *item = m_listLayout->itemAt(i);
        QWidget *w = item ? item->widget() : nullptr;
        if (!w || w == m_emptyLabel)
            continue;
        const QString id = w->property("sessionId").toString();
        const bool sel = (id == m_selectedId);
        w->setProperty("selected", sel);
        // 强制重绘 QSS
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
}

void ControlPanel::setPanelVisible(bool show)
{
    if (show)
        expand();
    else
        collapse();
}

bool ControlPanel::isPanelVisible() const
{
    return isVisible() && m_dockedExpanded;
}

void ControlPanel::setHostMinimized(bool minimized)
{
    if (minimized) {
        // 主窗口最小化: 面板随主窗口一起隐藏
        hide();
    } else if (m_dockedExpanded) {
        // 主窗口还原: 恢复展开显示
        expand();
    }
}

// ---- 内嵌面板 ----

void ControlPanel::expand()
{
    m_dockedExpanded = true;
    updateDockGeometry();
    if (isHidden())
        QWidget::show();
    raise();   // 覆盖在主窗口内容之上
    emit panelVisibilityChanged(true);
}

void ControlPanel::collapse()
{
    m_dockedExpanded = false;
    hide();
    emit panelVisibilityChanged(false);
}

void ControlPanel::updateDockGeometry()
{
    QWidget *host = parentWidget();
    if (!host) {
        hide();
        return;
    }
    const int h = qMin(600, host->height() - 24);
    const int y = (host->height() - h) / 2;
    // 顶层 Tool 窗:位置为屏幕全局坐标(子控件时代是主窗口相对坐标)
    // 展开: 面板覆盖在主窗口右侧内部(留 4px 呼吸空间)
    const QPoint topLeft = host->mapToGlobal(
        QPoint(host->width() - m_panelWidth - 4, y));
    setGeometry(topLeft.x(), topLeft.y(), m_panelWidth, h);
}

void ControlPanel::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    m_hideTimer->stop();
    if (!m_dockedExpanded)
        expand();
}

void ControlPanel::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_dockedExpanded)
        m_hideTimer->start();
}

bool ControlPanel::eventFilter(QObject *obj, QEvent *event)
{
    // 父窗口(主窗口)resize/移动 → 面板跟随重定位
    if (obj == parentWidget()) {
        if ((event->type() == QEvent::Resize || event->type() == QEvent::Move)
            && m_dockedExpanded) {
            updateDockGeometry();
            return false;
        }
        // 点击父窗口空白处(不在面板内) → 收回面板
        if (m_dockedExpanded && event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            // 顶层窗:geometry() 是全局坐标, 需用全局点击位置判断
            if (!geometry().contains(me->globalPosition().toPoint()))
                collapse();
        }
        return false;
    }
    // 列表卡片: 鼠标释放(点击) → 选中置顶
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto *w = qobject_cast<QWidget *>(obj)) {
            const QString id = w->property("sessionId").toString();
            if (!id.isEmpty()) {
                onItemClicked(id);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
