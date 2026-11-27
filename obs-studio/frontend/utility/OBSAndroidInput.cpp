/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "OBSAndroidInput.hpp"

#include <OBSApp.hpp>
#include <utility/android-window-fit.hpp>
#include <widgets/OBSAndroidDisplay.hpp>
#include <util/base.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGestureEvent>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QSlider>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <memory>
/* Android 触摸/按键补齐。平台把事件送进来了，但 OBS 那套桌面语义没人接 ——
 * 所以合在一个 app 级事件过滤器里：
 * ① 长按 = 右键。Android 上 Qt 把触摸转成鼠标事件，TapAndHold 手势在 QWidget 这条路上
 * 没人接、不会出上下文菜单，而 OBS 的场景/源/混音器菜单全挂在 customContextMenuRequested 上，
 * 没有右键就永远点不出来。
 * 不拦原始事件：按下照常发给控件（选中、滚动行为都不变），只是同时起一个计时器；到点还没松手
 * 也没滑出阈值，就照 Qt 处理 Key_Menu 的做法，给"最近的声明了上下文菜单策略的祖先控件"送一条
 * QContextMenuEvent(Mouse)。触发后的那次松手**只在真的弹起了菜单时**吞掉（免得给列表补一次"点击"）；
 * 什么都没弹就照常交回控件 —— 否则长按等于把按钮的 clicked 吃掉，见 fire() 的返回值注释。
 * ② 返回键 = Escape，见下面 KeyPress 那一格。
 * 不用 Q_OBJECT：只要 eventFilter 虚函数 + 成员 QTimer，省掉一次 moc。 */
/* 把顶层窗口压进屏幕可用区（全局兜底）。
 *
 * OBS 的对话框尺寸按桌面设计，手机横屏可用区更小，底部按钮和右侧内容会落在屏外、点不到。
 * 这里改成全局兜底：任何顶层窗口显示后，只要比可用区大就压到可用区并居中。
 * 覆盖全部对话框，以后新增的也自动受益。
 *
 * 两个注意点：
 * 1) 用 singleShot(0) 延迟一拍，不在 Show 里同步 resize —— Show 那一刻布局还没跑完，
 *    此时 size() 还是 .ui 里的设计尺寸，改完会被随后的布局覆盖。
 * 2) resize() 受控件最小尺寸约束（minimumSize / 布局的 minimumSizeHint）。
 *    压不下去时"实际"会大于"目标"——日志里两个数都打出来，一眼能看出是不是被挡住了。
 *    那种情况需要单独处理（内部套 QScrollArea，或放宽布局约束）。 */
static void clampTopLevelToScreen(QWidget *w)
{
	const AndroidFitResult r = androidFitTopLevelToScreen(w);
	if (!r.resized)
		return; /* 尺寸已经合适（或本来就不该压），不产生任何可见动作 */

	/* resize 被挡住，说明这个对话框还带着"桌面尺寸假设"（.ui 里写死的 minimumSize
	 * 或某个子控件的 minimumHeight），需要在它的构造函数里放宽 —— 见
	 * OBSBasicSourceSelect / OBSBasicSettings 的 #ifdef __ANDROID__ 块。
	 *
	 * 常见三类来源：
	 *   ① .ui 里写死 minimumHeight；
	 *   ② 布局首次 show 时把窗口自己的 minimumSize 设成布局最小尺寸
	 *      （QLayout 默认 SetDefaultConstraint），QWidget::resize() 被它夹住；
	 *   ③ QLayout 算最小尺寸走 qSmartMinSize()（qlayoutengine.cpp:321-333）：
	 *      sizePolicy 没有 ShrinkFlag 时取 max(sizeHint, minimumSizeHint)。
	 * ②③ 都能用"在源码里放宽最小尺寸"绕开，不需要去碰这些机制。 */
	if (r.actual != r.target) {
		/* 诊断：找出最占高度的那个子控件，够定位了（不再排前 5 名）。 */
		QString worst;
		int worstH = 0;
		for (QWidget *c : w->findChildren<QWidget *>()) {
			const int h = c->minimumSizeHint().height();
			if (h > worstH) {
				worstH = h;
				worst = QStringLiteral("%1/%2 minH=%3 sizeHint=%4")
						.arg(QString::fromLatin1(c->metaObject()->className()),
						     c->objectName())
						.arg(c->minimumHeight())
						.arg(c->sizeHint().height());
			}
		}

		/* 这里**故意不再硬压**。用 setMinimumSize(0,0)+SetNoConstraint 把窗口
		 * 强行缩到目标尺寸，会造出"窗口比内容小"的不一致状态：布局仍按原来的最小
		 * 尺寸摆放子控件，控件被裁在窗口外，列表的滚动和命中判定都会出错。
		 *
		 * 正确做法是在对话框自己的构造函数里放宽最小尺寸，参考
		 * OBSBasicSourceSelect / OBSBasicSettings 里的 #ifdef __ANDROID__ 块。
		 * 下面这行就是待办清单：出现了就说明该对话框还没做源码级修复。 */
		obsAndroidDisplayLog(
			"CLAMP-BLOCKED %s：目标 %dx%d 实际 %dx%d；最占高度=%s —— 该对话框需要在源码里放宽最小尺寸",
			w->metaObject()->className(), r.target.width(), r.target.height(), r.actual.width(),
			r.actual.height(), worst.toUtf8().constData());
	}

	obsAndroidDisplayLog("CLAMP %s %dx%d -> 目标 %dx%d 实际 %dx%d（可用 %dx%d）", w->metaObject()->className(),
			     r.before.width(), r.before.height(), r.target.width(), r.target.height(),
			     r.actual.width(), r.actual.height(), r.avail.width(), r.avail.height());
}

/* 为滚动区启用单指触摸滑动（QScroller + AndroidTouchScrollGuard）。
 *
 * 不直接挂 QScroller::grabGesture(LeftMouseButtonGesture) + MousePressEventDelay：
 *   Qt 6 内置单例 PressDelayHandler::sendingEvent 在派发单击触发的模态循环
 *   （例如点控制按钮里的“设置”进入 OBSBasicSettings::exec()）期间全程保持 true，
 *   会导致弹窗内的所有 QScroller 被全局 shouldEventBeIgnored() 吞掉而无法滑动。
 *   改为仅配置 QScroller 实例，统一由 AndroidTouchScrollGuard 负责 150ms 延迟按下、
 *   拖拽防误触、以及 QSlider 垂直/水平方向分流；同时关闭越界回弹移位。 */
static bool canScrollAxis(const QScrollBar *bar)
{
	return bar && bar->maximum() > bar->minimum();
}

static bool canScrollAreaAnyDirection(const QAbstractScrollArea *area)
{
	return area && (canScrollAxis(area->horizontalScrollBar()) || canScrollAxis(area->verticalScrollBar()));
}

static const QComboBox *comboBoxForPopup(const QWidget *popup)
{
	if (!popup || popup->windowType() != Qt::Popup)
		return nullptr;
	const auto *combo = qobject_cast<const QComboBox *>(popup->parentWidget());
	return combo && combo->view()->window() == popup ? combo : nullptr;
}

static bool isAndroidScrollTargetAllowed(const QWidget *area)
{
	if (!area)
		return false;
	if (const auto *itemView = qobject_cast<const QAbstractItemView *>(area)) {
		// Preserve single-finger drag-and-drop reordering in lists like Scenes and Sources.
		if (itemView->dragEnabled() && itemView->dragDropMode() != QAbstractItemView::NoDragDrop)
			return false;
	}
	const QWidget *win = area->window();
	if (!win)
		return true;
	if (win->windowType() == Qt::ToolTip || win->inherits("QDockWidgetGroupWindow"))
		return false;
	if (win->windowType() == Qt::Popup)
		return comboBoxForPopup(win) != nullptr;
	return true;
}

static QScroller *flickScrollerForArea(QAbstractScrollArea *area)
{
	if (!isAndroidScrollTargetAllowed(area))
		return nullptr;

	QWidget *vp = area->viewport();
	if (!vp)
		return nullptr;
	if (QScroller::hasScroller(vp))
		return QScroller::scroller(vp);

	if (auto *itemView = qobject_cast<QAbstractItemView *>(area)) {
		itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
		itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	}

	QScroller *scroller = QScroller::scroller(vp);
	if (!scroller)
		return nullptr;
	QScrollerProperties props = scroller->scrollerProperties();
	props.setScrollMetric(QScrollerProperties::DragStartDistance, 0.002);
	props.setScrollMetric(QScrollerProperties::MousePressEventDelay, 0.0);
	props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
	props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
	scroller->setScrollerProperties(props);
	return scroller;
}

static void fitPopupToSafeArea(QWidget *popup, const QWidget *anchor, int paddingPx, bool clampSize)
{
	if (!popup || !popup->isVisible())
		return;
	const QWidget *ref = anchor ? anchor : popup;
	const QScreen *scr = ref->screen() ? ref->screen()
					   : (ref->window() ? ref->window()->screen() : QGuiApplication::primaryScreen());
	if (!scr)
		return;
	QRect available = scr->availableGeometry();
	const QWindow *host = nullptr;
	if (anchor && anchor->window())
		host = anchor->window()->windowHandle();
	else if (popup->windowHandle() && popup->windowHandle()->transientParent())
		host = popup->windowHandle()->transientParent();
	else if (QWidget *active = QApplication::activeWindow())
		host = active->windowHandle();
	if (host) {
		// Dialogs and menus can open near screen edges or in narrow multi-window bounds.
		while (host->transientParent()) {
			host = host->transientParent();
		}
		const QMargins safe = obsAndroidWindowSafeMargins(host, scr);
		available = available.intersected(host->geometry().marginsRemoved(safe));
	} else {
		const QMargins safe = obsAndroidWindowSafeMargins(nullptr, scr);
		available = available.marginsRemoved(safe);
	}
	if (paddingPx > 0)
		available = available.marginsRemoved(QMargins(paddingPx, paddingPx, paddingPx, paddingPx));
	if (available.isEmpty())
		return;

	QRect geometry = popup->geometry();
	if (clampSize)
		geometry.setSize(geometry.size().boundedTo(available.size()));
	if (geometry.width() <= available.width()) {
		geometry.moveLeft(qBound(available.left(), geometry.left(), available.right() - geometry.width() + 1));
	} else {
		geometry.moveLeft(available.left());
	}
	if (geometry.height() <= available.height()) {
		geometry.moveTop(qBound(available.top(), geometry.top(), available.bottom() - geometry.height() + 1));
	} else {
		geometry.moveTop(available.top());
	}
	if (geometry != popup->geometry())
		popup->setGeometry(geometry);
}

static void fitComboBoxPopup(QWidget *popup, const QComboBox *combo)
{
	fitPopupToSafeArea(popup, combo, 8, true);

	QAbstractItemView *view = combo->view();
	flickScrollerForArea(view);
	view->scrollTo(view->currentIndex());
}

class AndroidTouchScrollGuard : public QObject {
public:
	static AndroidTouchScrollGuard *&globalSlot()
	{
		static AndroidTouchScrollGuard *guard = nullptr;
		return guard;
	}

	static AndroidTouchScrollGuard *current() { return globalSlot(); }

	bool isCancelRelease(const QEvent *e) const
	{
		return cancelingScroll && e && e->type() == QEvent::MouseButtonRelease;
	}

	explicit AndroidTouchScrollGuard(QObject *parent) : QObject(parent)
	{
		monotonicClock.start();
		holdTimer.setSingleShot(true);
		connect(&holdTimer, &QTimer::timeout, this, [this] {
			if (pendingTarget && qApp)
				promotePendingToWidget(static_cast<QApplication *>(qApp));
		});
	}

	~AndroidTouchScrollGuard() override
	{
		if (globalSlot() == this)
			globalSlot() = nullptr;
	}

	bool beforeNotify(QApplication *app, QObject *receiver, QEvent *e, bool &outResult)
	{
		if (replayingEvent == e)
			return false;

		const QEvent::Type type = e->type();
		if (type == QEvent::ApplicationStateChange) {
			if (QGuiApplication::applicationState() != Qt::ApplicationActive)
				reset();
			return false;
		}
		if (type == QEvent::WindowDeactivate || type == QEvent::Hide) {
			QWidget *trackedWin = activeWindow();
			if (trackedWin && receiver == trackedWin)
				reset();
			return false;
		}
		if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
		    type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
			return false;

		auto *me = static_cast<QMouseEvent *>(e);

		if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
			return handlePress(me, receiver, outResult);
		if (type == QEvent::MouseMove)
			return handleMove(app, me, outResult);
		return handleRelease(app, receiver, me, outResult);
	}

	void afterNotify(QEvent *e)
	{
		if (replayingEvent == e)
			return;
		if (e->type() == QEvent::MouseButtonRelease) {
			activeHoldTarget.clear();
			postHoldTarget.clear();
			postHoldArea.clear();
			postHoldViewport.clear();
		}
	}

private:
	static constexpr int HOLD_DELAY_MS = 150;
	static constexpr int AXIS_SLOP_LOGICAL_PX = 10;
	static constexpr int POST_HOLD_SLOP_LOGICAL_PX = 18;

	template<typename T> struct ScopedAssign {
		T &slot;
		T prev;
		explicit ScopedAssign(T &s, T v) : slot(s), prev(s) { slot = v; }
		~ScopedAssign() { slot = prev; }
	};

	QWidget *activeWindow() const
	{
		QWidget *w = pendingTarget
				     ? pendingTarget.data()
				     : (postHoldTarget ? postHoldTarget.data()
						       : (activeHoldTarget ? activeHoldTarget.data()
									   : scrollerTarget.data()));
		return w ? w->window() : nullptr;
	}

	static QWidget *findScrollTarget(QWidget *leaf, QAbstractScrollArea **outArea)
	{
		if (outArea)
			*outArea = nullptr;
		if (!leaf)
			return nullptr;
		if (QWidget *pop = QApplication::activePopupWidget(); pop && !pop->isAncestorOf(leaf) && pop != leaf)
			return nullptr;
		for (QWidget *cur = leaf; cur; cur = cur->isWindow() ? nullptr : cur->parentWidget()) {
			auto *area = qobject_cast<QAbstractScrollArea *>(cur->parentWidget());
			if (!area || area->viewport() != cur)
				continue;
			if (!isAndroidScrollTargetAllowed(area))
				return nullptr;
			if (canScrollAreaAnyDirection(area)) {
				if (outArea)
					*outArea = area;
				return cur;
			}
		}
		return nullptr;
	}

	static bool canHoldTransitionToScroll(const QWidget *target, const QAbstractScrollArea *area)
	{
		if (!target || qobject_cast<const QAbstractItemView *>(area))
			return false;
		return !qobject_cast<const QSlider *>(target) && !qobject_cast<const QComboBox *>(target) &&
		       !qobject_cast<const QAbstractSpinBox *>(target) && !qobject_cast<const QLineEdit *>(target) &&
		       !qobject_cast<const QTextEdit *>(target) && !qobject_cast<const QPlainTextEdit *>(target);
	}

	static std::unique_ptr<QMouseEvent> cloneMouseEvent(const QMouseEvent *me)
	{
		return std::unique_ptr<QMouseEvent>(static_cast<QMouseEvent *>(me->clone()));
	}

	void dispatchReplayedEvent(QApplication *app, QWidget *target, QEvent *ev)
	{
		ScopedAssign<QEvent *> scope(replayingEvent, ev);
		app->QApplication::notify(target, ev);
	}

	void clearPending()
	{
		holdTimer.stop();
		heldPress.reset();
		pendingTarget.clear();
		pendingSlider.clear();
		pendingArea.clear();
		pendingViewport.clear();
	}

	bool handlePress(const QMouseEvent *me, QObject *receiver, bool &outResult)
	{
		if (me->button() != Qt::LeftButton)
			return false;
		reset();

		auto *widget = qobject_cast<QWidget *>(receiver);
		if (!widget)
			return false;

		QAbstractScrollArea *area = nullptr;
		QWidget *vp = findScrollTarget(widget, &area);
		if (!area || !vp)
			return false;

		QScroller *scroller = QScroller::hasScroller(vp) ? QScroller::scroller(vp) : nullptr;
		if (scroller && scroller->state() != QScroller::Inactive) {
			const bool consumed = scroller->handleInput(QScroller::InputPress,
								    vp->mapFromGlobal(me->globalPosition()),
								    monotonicClock.elapsed());
			if (consumed && scroller->state() != QScroller::Inactive) {
				scrollerTarget = vp;
				outResult = true;
				return true;
			}
			if (scroller->state() != QScroller::Inactive)
				scroller->stop();
		}

		if (auto *slider = qobject_cast<QSlider *>(widget); slider && slider->isEnabled()) {
			const bool canScrollPerp =
				(slider->orientation() == Qt::Horizontal)
					? canScrollAxis(area->verticalScrollBar())
					: canScrollAxis(area->horizontalScrollBar());
			if (!canScrollPerp) {
				activeHoldTarget = slider;
				return false;
			}
			pendingSlider = slider;
		}

		pendingTarget = widget;
		pendingArea = area;
		pendingViewport = vp;
		pressGlobal = me->globalPosition().toPoint();
		pressGlobalF = me->globalPosition();
		pressElapsedMs = monotonicClock.elapsed();
		heldPress = cloneMouseEvent(me);
		// Keep the press pending in dropdowns so a slow drag cannot start item selection.
		if (!comboBoxForPopup(area->window()))
			holdTimer.start(HOLD_DELAY_MS);
		outResult = true;
		return true;
	}

	bool handleMove(QApplication *app, const QMouseEvent *me, bool &outResult)
	{
		if (activeHoldTarget)
			return false;
		if (scrollerTarget) {
			QScroller::scroller(scrollerTarget)
				->handleInput(QScroller::InputMove,
					      scrollerTarget->mapFromGlobal(me->globalPosition()),
					      monotonicClock.elapsed());
			outResult = true;
			return true;
		}
		if (pendingTarget) {
			if (!pendingViewport || !heldPress) {
				reset();
				return false;
			}
			const QPoint delta = me->globalPosition().toPoint() - pressGlobal;
			const int dx = qAbs(delta.x());
			const int dy = qAbs(delta.y());
			if (dx + dy < AXIS_SLOP_LOGICAL_PX) {
				outResult = true;
				return true;
			}
			if (pendingSlider && pendingSlider->isEnabled()) {
				const bool alongSlider =
					(pendingSlider->orientation() == Qt::Horizontal) ? (dx >= dy) : (dy >= dx);
				if (alongSlider) {
					promotePendingToWidget(app);
					return false;
				}
			}
			if (!promotePendingToScroller(me)) {
				promotePendingToWidget(app);
				return false;
			}
			outResult = true;
			return true;
		}
		if (postHoldTarget) {
			if (!postHoldViewport || !postHoldArea || QApplication::activePopupWidget()) {
				postHoldTarget.clear();
				postHoldArea.clear();
				postHoldViewport.clear();
				return false;
			}
			const QPoint delta = me->globalPosition().toPoint() - pressGlobal;
			if (qAbs(delta.x()) + qAbs(delta.y()) < POST_HOLD_SLOP_LOGICAL_PX)
				return false;
			if (promotePostHoldToScroller(app, me)) {
				outResult = true;
				return true;
			}
			return false;
		}
		return false;
	}

	bool handleRelease(QApplication *app, QObject *receiver, const QMouseEvent *me, bool &outResult)
	{
		if (scrollerTarget) {
			QPointer<QWidget> vp = scrollerTarget;
			scrollerTarget.clear();
			if (vp) {
				QScroller::scroller(vp)->handleInput(QScroller::InputRelease,
								     vp->mapFromGlobal(me->globalPosition()),
								     monotonicClock.elapsed());
			}
			outResult = true;
			return true;
		}
		if (pendingTarget) {
			QPointer<QWidget> target = pendingTarget;
			QPointer<QObject> recGuard(receiver);
			auto press = std::move(heldPress);
			clearPending();
			if (target && press)
				dispatchReplayedEvent(app, target, press.get());
			if (!recGuard) {
				outResult = true;
				return true;
			}
			return false;
		}
		return false;
	}

	void promotePendingToWidget(QApplication *app)
	{
		QPointer<QWidget> target = pendingTarget;
		QPointer<QAbstractScrollArea> area = pendingArea;
		QPointer<QWidget> vp = pendingViewport;
		auto press = std::move(heldPress);
		clearPending();
		if (!target)
			return;
		if (canHoldTransitionToScroll(target, area) && vp) {
			postHoldTarget = target;
			postHoldArea = area;
			postHoldViewport = vp;
		} else {
			activeHoldTarget = target;
		}
		if (press)
			dispatchReplayedEvent(app, target, press.get());
	}

	bool startScrollerDrag(QWidget *vp, QAbstractSlider *slider, const QPointF &moveGlobalPos)
	{
		if (!vp)
			return false;
		// Hidden pages and untouched controls do not need a scroller.
		auto *scroller = flickScrollerForArea(qobject_cast<QAbstractScrollArea *>(vp->parentWidget()));
		if (!scroller)
			return false;
		if (!slider) {
			if (QWidget *child = vp->childAt(vp->mapFromGlobal(pressGlobalF.toPoint()))) {
				if (!qobject_cast<QScrollBar *>(child))
					slider = qobject_cast<QAbstractSlider *>(child);
			}
		}
		QPointer<QAbstractSlider> sliderGuard(slider);
		const bool wasTransparent = slider && slider->testAttribute(Qt::WA_TransparentForMouseEvents);
		// QAbstractScrollAreaPrivate::canStartScrollingAt rejects ScrollPrepare over a QAbstractSlider.
		if (slider && !wasTransparent)
			slider->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		scroller->handleInput(QScroller::InputPress, vp->mapFromGlobal(pressGlobalF), pressElapsedMs);
		if (sliderGuard && !wasTransparent)
			sliderGuard->setAttribute(Qt::WA_TransparentForMouseEvents, false);
		if (scroller->state() == QScroller::Inactive)
			return false;
		scrollerTarget = vp;
		scroller->handleInput(QScroller::InputMove, vp->mapFromGlobal(moveGlobalPos),
				      monotonicClock.elapsed());
		return true;
	}

	bool promotePendingToScroller(const QMouseEvent *moveEvent)
	{
		if (!startScrollerDrag(pendingViewport, pendingSlider, moveEvent->globalPosition()))
			return false;
		clearPending();
		return true;
	}

	bool promotePostHoldToScroller(QApplication *app, const QMouseEvent *moveEvent)
	{
		QPointer<QWidget> target = postHoldTarget;
		QPointer<QWidget> vp = postHoldViewport;
		postHoldTarget.clear();
		postHoldArea.clear();
		postHoldViewport.clear();
		if (target) {
			// Cancel button highlight and long-press timer without triggering clicked() or context menus.
			const QPointF farAway(-1e6, -1e6);
			QMouseEvent cancelRelease(QEvent::MouseButtonRelease, farAway, farAway, farAway, Qt::LeftButton,
						  Qt::NoButton, moveEvent->modifiers(), moveEvent->source(),
						  moveEvent->pointingDevice());
			cancelRelease.setTimestamp(moveEvent->timestamp());
			ScopedAssign<bool> cancelScope(cancelingScroll, true);
			dispatchReplayedEvent(app, target, &cancelRelease);
		}
		return startScrollerDrag(vp, nullptr, moveEvent->globalPosition());
	}

	void reset()
	{
		clearPending();
		activeHoldTarget.clear();
		postHoldTarget.clear();
		postHoldArea.clear();
		postHoldViewport.clear();
		if (scrollerTarget) {
			QScroller *scroller = QScroller::scroller(scrollerTarget);
			if (scroller && scroller->state() == QScroller::Dragging)
				scroller->stop();
		}
		scrollerTarget.clear();
	}

	QTimer holdTimer;
	QElapsedTimer monotonicClock;
	QPointer<QWidget> pendingTarget;
	QPointer<QSlider> pendingSlider;
	QPointer<QAbstractScrollArea> pendingArea;
	QPointer<QWidget> pendingViewport;
	QPointer<QWidget> activeHoldTarget;
	QPointer<QWidget> postHoldTarget;
	QPointer<QAbstractScrollArea> postHoldArea;
	QPointer<QWidget> postHoldViewport;
	QPointer<QWidget> scrollerTarget;
	QPoint pressGlobal;
	QPointF pressGlobalF;
	qint64 pressElapsedMs = 0;
	std::unique_ptr<QMouseEvent> heldPress;
	QEvent *replayingEvent = nullptr;
	bool cancelingScroll = false;
};

class AndroidInputFilter : public QObject {
public:
	explicit AndroidInputFilter(QObject *parent = nullptr) : QObject(parent)
	{
		timer.setSingleShot(true);
		/* 到点只是"长按到位"，不当场弹菜单 —— OBS 的上下文菜单结尾是阻塞的 popup.exec()，
		 * 手指还按着的时候弹出去，会被随后那发 touch 当成"点在菜单外"立刻 dismiss。
		 * 真正的弹出发放延到松手那一刻。 */
		connect(&timer, &QTimer::timeout, this, [this] { armed = true; });
	}

protected:
	bool eventFilter(QObject *watched, QEvent *ev) override
	{
		if (!watched->isWidgetType())
			return false;

		switch (ev->type()) {
		/* Android 的返回键 = Escape。BACK 以 QEvent::KeyPress、key=Qt::Key_Back 正常进来，
		 * 沿焦点链一路传播到底没人接，而前端全树对 Qt::Key_Back 零处理 ⇒ 缺的只有"翻译"这一环。
		 * 为什么换成 Escape 而不是直接 close()/reject()：投影仪的关闭链是
		 * EscapeTriggered → OBSBasic::DeleteProjector（键盘 :65 与触摸菜单 :310 两条路都走它），
		 * 自己 close() 会绕过它；而根界面上浮动 dock 之类也是顶层窗，close() 会把它直接收掉。
		 * 送 Escape 就等于把"桌面上按 Escape 会发生什么"原样搬过来：QDialog 靠 app_event_filter:266
		 * 放行 + 它自己的 keyPressEvent reject，投影仪/Multiview 靠那条快捷键。 */
		case QEvent::KeyPress: {
			QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() != Qt::Key_Back)
				break;
			/* 菜单/popup 立着时 Qt 自己就把它撤了，不抢。 */
			if (QApplication::activePopupWidget())
				break;
			QWidget *top = static_cast<QWidget *>(watched)->window();
/* 焦点还在根界面上 ⇒ BACK 留给 Android；"退回后台"是另一笔账，不混进这一条里改。 */
			if (top == static_cast<OBSApp *>(qApp)->GetMainWindow())
				break;
			backEaten = true;
			QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
			QApplication::sendEvent(top, &esc);
			blog(LOG_DEBUG, "AndroidInputFilter: Back 键转换为 Escape 发送至 %s（modal=%d，accepted=%d）",
			     top->metaObject()->className(), (int)top->isModal(), (int)esc.isAccepted());
			return true;
		}
		case QEvent::KeyRelease:
			/* 按下那拍已经当成 Escape 用掉了；留着这次松手只会再走一遍"BACK 没人处理"。 */
			if (backEaten && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Back) {
				backEaten = false;
				return true;
			}
			break;
		case QEvent::Show:
		case QEvent::Hide:
		case QEvent::Close:
		case QEvent::Expose: {
			QWidget *pw = static_cast<QWidget *>(watched);
			if (!pw->isWindow() || pw == static_cast<OBSApp *>(qApp)->GetMainWindow())
				break;
			const Qt::WindowFlags flags = pw->windowFlags();
			const QRect geo = pw->geometry();
			const char *kind = ev->type() == QEvent::Show
						   ? "SHOW"
						   : ev->type() == QEvent::Hide
							     ? "HIDE"
							     : ev->type() == QEvent::Close ? "CLOSE" : "EXPOSE";
			obsAndroidDisplayLog(
				"P8-W %s cls=%s obj=%s geo=%dx%d+%d+%d popup=%d tool=%d dialog=%d vis=%d act=%d", kind,
				pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(), geo.y(),
				geo.width(), geo.height(), int(flags.testFlag(Qt::Popup)), int(flags.testFlag(Qt::Tool)),
				int(flags.testFlag(Qt::Dialog)), int(pw->isVisible()),
				int(qApp->activePopupWidget() == pw));

			/* 等 Qt 完成布局后，约束下拉框、上下文菜单和普通对话框的位置。 */
			if (ev->type() == QEvent::Show) {
				QTimer::singleShot(0, pw, [pw] {
					if (pw->windowType() == Qt::Popup) {
						const auto *combo = comboBoxForPopup(pw);
						if (combo && pw->isVisible()) {
							fitComboBoxPopup(pw, combo);
						} else if (qobject_cast<QMenu *>(pw) && pw->isVisible()) {
							fitPopupToSafeArea(pw, pw->parentWidget(), 0, false);
						}
						return;
					}
					clampTopLevelToScreen(pw);
				});
			}
			break;
		}
		case QEvent::Gesture: {
			QGestureEvent *ge = static_cast<QGestureEvent *>(ev);
			if (QGesture *g = ge->gesture(Qt::TapAndHoldGesture))
				obsAndroidDisplayLog("P8-G tapandhold state=%d recv=%s accepted=%d", (int)g->state(),
						       watched->metaObject()->className(), int(ev->isAccepted()));
			break;
		}
		case QEvent::MouseButtonPress: {
			QWidget *pw = static_cast<QWidget *>(watched);
			const QRect geo = pw->geometry();
			obsAndroidDisplayLog("P8-PRESS cls=%s obj=%s geo=%dx%d+%d+%d combo=%d win=%s",
					     pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(),
					     geo.y(), geo.width(), geo.height(),
					     int(qobject_cast<QComboBox *>(pw) != nullptr),
					     pw->window() ? pw->window()->metaObject()->className() : "?");
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if (me->button() != Qt::LeftButton)
				break;
			/* 组合框弹层：Qt 会在按下时自己建弹层，但这发松手送到的是主窗的 widget，不是刚建出来的
			 * 弹层，`QComboBoxPrivateContainer::eventFilter` 对落在弹层外的 MouseButtonRelease
			 * 一律 hidePopup ⇒ 弹层"开在松手之前的 1 ms 里、又被这发松手关掉"。
			 * ⇒ 最小干预：**不替 Qt 弹**，只记下"这发按下落在组合框上"，
			 *   到松手那一刻再看弹层立没立着（见下面 release 分支）。
			 * 判定必须放到松手时：按下时读 `activePopupWidget()` 恒为 null
			 *   （Qt 登记活动弹层要到下一轮事件循环）。
			 * 弹层已经立着时不记：那一下该由 Qt 的"点外面就关"收尾，不抢。
			 * 长按那套（armed/timer）这里一并跳过：弹层刚立起来，再补一发合成右键只会把它按死。 */
			if (QComboBox *cb = qobject_cast<QComboBox *>(pw)) {
				if (!qApp->activePopupWidget())
					comboPress = cb;
				break;
			}
			if (comboBoxForPopup(pw->window())) {
				timer.stop();
				armed = false;
				target.clear();
				break;
			}
			target = static_cast<QWidget *>(watched);
			pressGlobal = me->globalPosition().toPoint();
			armed = false;
			timer.start(HOLD_MS);
			break;
		}
		case QEvent::MouseMove: {
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if ((me->globalPosition().toPoint() - pressGlobal).manhattanLength() > MOVE_SLOP) {
				timer.stop(); // 这是拖动/滚动，不是长按
				armed = false;
			}
			break;
		}
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			if (ev->type() == QEvent::MouseButtonRelease) {
				if (auto *guard = AndroidTouchScrollGuard::current();
				    guard && guard->isCancelRelease(ev)) {
					timer.stop();
					armed = false;
					comboPress = nullptr;
					target.clear();
					return false;
				}
				QMouseEvent *re = static_cast<QMouseEvent *>(ev);
				QWidget *rp = static_cast<QWidget *>(watched);
				QWidget *pop = qApp->activePopupWidget();
				blog(LOG_DEBUG, "AndroidInputFilter: mouseRelease cls=%s at=(%d,%d) activePopup=%s consumed=%d",
				     rp->metaObject()->className(), re->globalPosition().toPoint().x(),
				     re->globalPosition().toPoint().y(), pop ? pop->metaObject()->className() : "none",
				     int(comboPress != nullptr));
			}
			/* 这发松手如果正好是"刚落在组合框上的那次按下"的尾巴、
			 * 而此刻弹层确实立着，就把它吃掉 —— Qt 那条"松手落在弹层外 ⇒ hidePopup"收不到事件，弹层留住。
			 * 判定放在这里（而不是按下时）是因为活动弹层要等下一轮事件循环才登记得上，见上面那段。
			 * 指针一进分支就清 ⇒ 最多多吃一发，绝不会波及后面不相干的松手（拖滚动条那类）。 */
			if (comboPress) {
				QWidget *pop = qApp->activePopupWidget();
				comboPress = nullptr;
				if (pop && pop->inherits("QComboBoxPrivateContainer")) {
					obsAndroidDisplayLog("P8-EAT 忽略本次触摸释放，保持下拉框打开：%s", pop->metaObject()->className());
					return true;
				}
			}
			timer.stop();
			if (armed) {
				armed = false;
				/* 只有真的弹起了菜单才吞这发松手。吞的理由（对列表那类成立）：既不给它补一次
				 * "点击"，也不让它以为按键还开着。但**没弹出任何东西的长按**本来就是"一次按得
				 * 久一点的点击" —— 一律吞掉就把只有 `clicked` 一个入口的按钮判了死刑。 */
				if (!fire())
					return false;
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

private:
	static constexpr int HOLD_MS = 500;
	static constexpr int MOVE_SLOP = 24;

	/* 返回值 = "这发长按真的立起过菜单吗"。松手分支要用（见 eventFilter 里 MouseButtonRelease
	 * 那段）：立过才吞松手，没立就把它交回控件 —— 否则只有 `clicked` 一个入口的按钮
	 * 在手指按久一点时变成"点了没反应"。三条出口各自量耗时 > 0 就算立过 ——
	 * OBS 的菜单结尾是阻塞的 popup.exec()，同毫秒返回的那条路根本没弹东西。 */
	bool fire()
	{
		QWidget *w = target.data();
		if (!w)
			return false;

		/* 按下事件落在的是 QAbstractScrollArea 的 viewport，而策略设在列表控件自己身上；
		 * 往上找到那个声明了策略的祖先，pos 也按它的坐标系给 —— OBS 的 handler 里是 itemAt(pos)。
		 * 另外补一次**向下的几何落点**：预览区的 `Qt::CustomContextMenu` 设在它**自己身上**
		 * （`forms/OBSBasic.ui:240`），既不在 w 上、也不在 w 的祖先里 ⇒ 只往上爬永远找不到它。
		 * `childAt` 是纯几何的下钻（不看事件派发怎么选接收者），拿同一个 pressGlobal 在 w 的子树里
		 * 再落一次点。**只在往下落空了之后才走老路** `policyUp(w->parentWidget())`。 */
		auto policyUp = [](QWidget *x) {
			while (x) {
				const Qt::ContextMenuPolicy policy = x->contextMenuPolicy();
				if (policy == Qt::CustomContextMenu || policy == Qt::ActionsContextMenu)
					return x;
				x = x->parentWidget();
			}
			return static_cast<QWidget *>(nullptr);
		};
		QWidget *const hit = w->childAt(w->mapFromGlobal(pressGlobal));
		QWidget *menu = hit ? policyUp(hit) : nullptr;
		if (!menu)
			menu = policyUp(w->parentWidget());
		if (!menu)
			menu = w;

		/* 日志里补 objectName、几何、是否顶层窗及所属顶层窗的 objectName：只有 className 时
		 * 无法区分"长按没触发"和"触发了但控件没挂菜单"，也自证不了接收者身份 ——
		 * 预览中心的落点报的就是 OBSBasic，可落点其实在显示面自己的原生子窗里。
		 * 这些字段一律追加在行尾，前缀格式保持不变。 */
		const QRect wGeo = w->geometry();
		const QWidget *wWin = w->window();
		blog(LOG_DEBUG,
		     "AndroidInputFilter: 长按菜单目标 %s@(%d,%d) -> %s%s [obj='%s' geo=%dx%d+%d+%d isWindow=%d root='%s']",
		     w->metaObject()->className(), pressGlobal.x(), pressGlobal.y(), menu->metaObject()->className(),
		     menu == w ? "（回退为事件源控件）" : "", qUtf8Printable(w->objectName()), wGeo.width(),
		     wGeo.height(), wGeo.x(), wGeo.y(), int(w->isWindow()),
		     qUtf8Printable(wWin ? wWin->objectName() : QString()));

		const QString hitDesc = hit ? QStringLiteral("'%1' %2").arg(hit->objectName(),
									     QString::fromUtf8(
										     hit->metaObject()->className()))
					    : QStringLiteral("none");
		const QByteArray hitUtf8 = hitDesc.toUtf8();
		const QByteArray tailUtf8 =
			(menu == w ? QStringLiteral("（未匹配到上下文菜单策略持有者）") : QString()).toUtf8();
		blog(LOG_DEBUG, "AndroidInputFilter: 长按命中 childAt=%s，采用='%s'%s", hitUtf8.constData(),
		     qUtf8Printable(menu->objectName()), tailUtf8.constData());

		/* 坐标系：QAbstractScrollArea 那一类（SourceTree/SceneTree 都是）的
		 * `customContextMenuRequested` 按 Qt 文档给的是 **viewport 系**坐标 —— OBS 的 handler 里
		 * 就是 `itemAt(pos)`，而 `menu` 是带 2px 边框的外层控件。照外层算会整体偏一个边框宽，
		 * 行边界上这一偏还会选错行。 */
		QWidget *coordBase = menu;
		if (QAbstractScrollArea *scroll = qobject_cast<QAbstractScrollArea *>(menu))
			coordBase = scroll->viewport();
		const QPoint local = coordBase->mapFromGlobal(pressGlobal);
		QContextMenuEvent ce(QContextMenuEvent::Mouse, local, pressGlobal);
		QElapsedTimer held;
		held.start();
		QApplication::sendEvent(menu, &ce);
		const long long spent = (long long)held.elapsed();
		blog(LOG_DEBUG, "AndroidInputFilter: QContextMenuEvent 返回耗时=%lldms，活动弹出菜单=%s", spent,
		     qApp->activePopupWidget() ? "存在" : "无");

		if (spent > 0)
			return true; // 阻塞过 = 菜单真的立起来过
		{
			blog(LOG_DEBUG, "AndroidInputFilter: 事件未打开菜单，触发 customContextMenuRequested(%d,%d)",
			     local.x(), local.y());
			QElapsedTimer held2;
			held2.start();
			const bool ok = QMetaObject::invokeMethod(menu, "customContextMenuRequested",
								  Q_ARG(QPoint, local));
			const long long spent2 = (long long)held2.elapsed();
			if (ok && spent2 > 0) {
				blog(LOG_DEBUG, "AndroidInputFilter: customContextMenuRequested 已处理（耗时=%lldms）",
				     spent2);
				return true;
			}
			blog(LOG_DEBUG,
			     "AndroidInputFilter: customContextMenuRequested 未触发菜单（ok=%d，耗时=%lldms），尝试发送右键事件至 %s",
			     ok, spent2, w->metaObject()->className());
			QElapsedTimer held3;
			held3.start();
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(pressGlobal),
					  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(w, &press); // 到这里阻塞，直到菜单被点掉
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(pressGlobal),
					    Qt::RightButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(w, &release);
			const long long spent3 = (long long)held3.elapsed();
			blog(LOG_DEBUG, "AndroidInputFilter: 右键菜单已关闭（耗时=%lldms）", spent3);
			return spent3 > 0;
		}
	}

	QTimer timer{this};
	QPointer<QWidget> target;
	QPoint pressGlobal;
	bool armed = false;
	bool backEaten = false;
	QPointer<QComboBox> comboPress; // 这发按下落在组合框上（松手时据此决定吃不吃）
};


void InstallAndroidAppFilters(QApplication *app)
{
	app->installEventFilter(new AndroidInputFilter(app));
	AndroidTouchScrollGuard::globalSlot() = new AndroidTouchScrollGuard(app);
}

bool AndroidBeforeAppNotify(QApplication *app, QObject *receiver, QEvent *e, bool &result)
{
	if (auto *guard = AndroidTouchScrollGuard::current(); guard && !QCoreApplication::closingDown())
		return guard->beforeNotify(app, receiver, e, result);
	return false;
}

void AndroidAfterAppNotify(QEvent *e)
{
	if (auto *guard = AndroidTouchScrollGuard::current(); guard && !QCoreApplication::closingDown())
		guard->afterNotify(e);
}
