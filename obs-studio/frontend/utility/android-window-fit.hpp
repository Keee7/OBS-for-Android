#pragma once

#ifdef __ANDROID__

#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>
#include <QtGlobal>
#include "android-layout-policy.hpp"

inline QRect androidAvailableWindowRect(const QWidget *w)
{
	const QScreen *scr = w ? w->screen() : nullptr;
	if (!scr)
		return {};
	QRect available = scr->availableGeometry();
	// Dialogs must also fit the current app window in Android multi-window
	// mode. QScreen alone may still describe the complete tablet display.
	const QWidget *parent = w->parentWidget();
	if (parent) {
		const QWidget *host = parent->window();
		const QRect hostRect(host->mapToGlobal(QPoint()), host->size());
		const QRect intersection = available.intersected(hostRect);
		if (!intersection.isEmpty())
			available = intersection;
	}
	return available;
}

/* 把一个顶层窗口缩进屏幕可用区并居中。
 *
 * 两个调用时机，逻辑同一份：
 *
 *   1. **show() 之前**（对话框构造函数里）—— 窗口第一次出现就是最终尺寸。
 *      Qt 的 show() 顺序是 ensurePolished() → show_sys()（创建并映射原生窗口）
 *      → 发送 QShowEvent。等 QShowEvent 到达时，窗口已经按 .ui 的设计尺寸画出来
 *      了，此时再 resize 用户就会看到"先大后小"闪一下。所以预先是唯一能消除
 *      闪烁的位置。
 *
 *   2. **show() 之后**（OBSApp 的全局兜底 clampTopLevelToScreen）—— 尺寸已经
 *      合适时这里直接早退（resized=false），不产生任何可见动作；只有还没做
 *      源码级修复的窗口才会被动，并被 CLAMP-BLOCKED 日志点名。
 *
 * 返回是否真的动过，以及期望/实际尺寸，供调用方打日志用。 */
struct AndroidFitResult {
	bool resized = false;
	QRect avail; /* 屏幕可用区（已扣掉状态栏等 inset） */
	QSize before;
	QSize target; /* 期望尺寸 */
	QSize actual; /* 实际尺寸：被最小尺寸挡住时会不等于 target */
};

inline AndroidFitResult androidFitTopLevelToScreen(QWidget *w)
{
	AndroidFitResult r;

	if (!w || !w->isWindow())
		return r;

	/* 弹层/工具提示不参与：它们的位置由 Qt 相对锚点算，硬压进来反而错位。 */
	const Qt::WindowFlags flags = w->windowFlags();
	if (flags.testFlag(Qt::Popup) || flags.testFlag(Qt::ToolTip))
		return r;

	const QScreen *scr = w->screen();
	if (!scr)
		return r;

	const QRect avail = androidAvailableWindowRect(w);
	if (avail.isEmpty())
		return r;
	const int margin = 8;
	const QSize maxSize(OBSAndroidLayout::fitLimit(avail.width(), margin),
			    OBSAndroidLayout::fitLimit(avail.height(), margin));

	const QSize before = w->size();
	r.avail = avail;
	r.before = before;

	/* 满屏窗口（投影仪 / Multiview）不要压：它们本来就该铺满，压到 avail-16
	 * 再居中的话四周会留一圈黑边。判据必须是"相等"而不是"不小于" —— 后者会把
	 * 981x512 的设置对话框也当成满屏窗，导致它完全不被压。 */
	if (w->isFullScreen() || before == scr->geometry().size())
		return r;

	QSize target = before;
	if (target.width() > maxSize.width())
		target.setWidth(maxSize.width());
	if (target.height() > maxSize.height())
		target.setHeight(maxSize.height());

	if (target == before)
		return r; /* 本来就装得下，不动 */

	w->resize(target);
	const QSize after = w->size();

	/* 装不下时别居中：居中会算出负的 y，标题栏跑出屏外、底部依旧点不到。
	 * 贴左上角至少标题栏和上半部分是可用的。 */
	if (after.width() > avail.width() || after.height() > avail.height())
		w->move(avail.topLeft());
	else
		w->move(avail.center() - QPoint(after.width() / 2, after.height() / 2));

	r.resized = true;
	r.target = target;
	r.actual = after;
	return r;
}

/* 紧凑窗口的内嵌预览按可用高取 1/5，夹在 [60, 120]；宽大窗口保留 150。
 *
 * 不能写死 80 —— 那是按 393 高的手机挑的：在 800 高的平板上只占 10%，
 * 白白浪费；在 288 高的矮屏上又占 28%，属性区就没地方了。
 * 拿不到屏幕信息时退回 80（原来的经验值）。 */
inline int androidPreviewMinHeight(const QWidget *w)
{
	const QRect available = androidAvailableWindowRect(w);
	const int availH = available.height();
	if (availH <= 0)
		return 80;
	if (!OBSAndroidLayout::compact(available.width(), availH))
		return 150;
	return qBound(60, availH / 5, 120);
}

#endif
