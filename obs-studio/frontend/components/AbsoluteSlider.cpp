#include "AbsoluteSlider.hpp"

#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

#include "moc_AbsoluteSlider.cpp"

namespace {
constexpr int kLightGrooveHeight = 4;
constexpr int kLightHandleWidth = 20;
constexpr int kLightHandleHeight = 10;
} // namespace

AbsoluteSlider::AbsoluteSlider(QWidget *parent) : SliderIgnoreScroll(parent)
{
	installEventFilter(this);
	setMouseTracking(true);

	tickColor.setRgb(0x5b, 0x62, 0x73);
}

AbsoluteSlider::AbsoluteSlider(Qt::Orientation orientation, QWidget *parent) : SliderIgnoreScroll(orientation, parent)
{
	installEventFilter(this);
	setMouseTracking(true);

	tickColor.setRgb(0x5b, 0x62, 0x73);
}

void AbsoluteSlider::mousePressEvent(QMouseEvent *event)
{
	dragging = (event->buttons() & Qt::LeftButton || event->buttons() & Qt::MiddleButton);

	if (dragging) {
		setSliderDown(true);
		const int val = posToRangeValue(event);
		if (lightweightMode)
			setProgressValue(val);
		else
			setValue(val);
		emit AbsoluteSlider::sliderMoved(val);
	}

	event->accept();
}

void AbsoluteSlider::mouseReleaseEvent(QMouseEvent *event)
{
	dragging = false;
	setSliderDown(false);
	event->accept();
}

void AbsoluteSlider::mouseMoveEvent(QMouseEvent *event)
{
	int val = posToRangeValue(event);

	if (val > maximum()) {
		val = maximum();
	} else if (val < minimum()) {
		val = minimum();
	}

	emit absoluteSliderHovered(val);

	if (dragging) {
		if (lightweightMode)
			setProgressValue(val);
		else
			setValue(val);
		emit AbsoluteSlider::sliderMoved(val);
	}

	QSlider::mouseMoveEvent(event);
	event->accept();
}

bool AbsoluteSlider::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

		if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
			return true;
		}
	}

	return QSlider::eventFilter(obj, event);
}

int AbsoluteSlider::posToRangeValue(QMouseEvent *event)
{
	if (lightweightMode && orientation() == Qt::Horizontal) {
		const QRect groove = lightweightGrooveRect();
		return style()->sliderValueFromPosition(minimum(), maximum(), event->pos().x() - groove.left(),
							groove.width(), false);
	}

	QStyleOptionSlider opt;
	initStyleOption(&opt);

	int pos;
	int sliderMin;
	int sliderMax;
	int handleLength;

	const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

	if (orientation() == Qt::Horizontal) {
		pos = event->pos().x();
		handleLength = handle.width();
		sliderMin = groove.left() + (handleLength / 2);
		sliderMax = groove.right() - (handleLength / 2) + 1;
	} else {
		pos = event->pos().y();
		handleLength = handle.height();
		sliderMin = groove.top() + (handleLength / 2);
		sliderMax = groove.bottom() - (handleLength / 2) + 1;
	}

	int sliderValue = style()->sliderValueFromPosition(minimum(), maximum(), pos - sliderMin, sliderMax - sliderMin,
							   opt.upsideDown);

	return sliderValue;
}

bool AbsoluteSlider::getDisplayTicks() const
{
	return displayTicks;
}

void AbsoluteSlider::setDisplayTicks(bool display)
{
	displayTicks = display;
}

QColor AbsoluteSlider::getTickColor() const
{
	return tickColor;
}

void AbsoluteSlider::setTickColor(QColor c)
{
	tickColor = std::move(c);
}

void AbsoluteSlider::setLightweightMode(bool enable)
{
	if (lightweightMode == enable)
		return;

	lightweightMode = enable;
	colorsReady = false;
	if (enable) {
		progressValue = value();
		setAttribute(Qt::WA_OpaquePaintEvent, true);
	} else {
		setAttribute(Qt::WA_OpaquePaintEvent, false);
		QSlider::setValue(progressValue);
	}
	update();
}

void AbsoluteSlider::setProgressValue(int val)
{
	const int clamped = std::clamp(val, minimum(), maximum());
	if (!lightweightMode) {
		setValue(clamped);
		return;
	}
	if (clamped == progressValue)
		return;

	const int oldVal = progressValue;
	progressValue = clamped;
	if (size().isEmpty())
		return;

	const QRect dirty = lightweightDirtyRect(oldVal, clamped);
	if (!dirty.isEmpty())
		update(dirty);
}

void AbsoluteSlider::changeEvent(QEvent *event)
{
	if (lightweightMode && event &&
	    (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
	     event->type() == QEvent::EnabledChange)) {
		colorsReady = false;
		update();
	}
	SliderIgnoreScroll::changeEvent(event);
}

void AbsoluteSlider::resizeEvent(QResizeEvent *event)
{
	if (lightweightMode)
		update();
	SliderIgnoreScroll::resizeEvent(event);
}

QRect AbsoluteSlider::lightweightGrooveRect() const
{
	const int pad = kLightHandleWidth / 2;
	const int trackW = std::max(1, width() - kLightHandleWidth);
	const int gy = (height() - kLightGrooveHeight) / 2;
	return QRect(pad, gy, trackW, kLightGrooveHeight);
}

int AbsoluteSlider::lightweightHandleCenterX(int val) const
{
	const int span = std::max(1, maximum() - minimum());
	const double frac = std::clamp(double(val - minimum()) / double(span), 0.0, 1.0);
	const QRect groove = lightweightGrooveRect();
	return groove.left() + int(std::llround(frac * groove.width()));
}

QRect AbsoluteSlider::lightweightDirtyRect(int oldVal, int newVal) const
{
	const int oldX = lightweightHandleCenterX(oldVal);
	const int newX = lightweightHandleCenterX(newVal);
	const int halfW = kLightHandleWidth / 2 + 3;
	const int loX = std::max(0, std::min(oldX, newX) - halfW);
	const int hiX = std::min(width(), std::max(oldX, newX) + halfW);
	const int boxH = std::max(kLightHandleHeight, kLightGrooveHeight) + 6;
	const int loY = std::max(0, (height() - boxH) / 2);
	const int hiY = std::min(height(), loY + boxH);
	return QRect(loX, loY, std::max(0, hiX - loX), std::max(0, hiY - loY));
}

void AbsoluteSlider::ensureLightweightColors()
{
	if (colorsReady)
		return;

	QColor bg = palette().color(QPalette::Window);
	for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
		QImage pImg(16, 16, QImage::Format_RGBA8888_Premultiplied);
		pImg.fill(Qt::transparent);
		QPainter pp(&pImg);
		QStyleOption pOpt;
		pOpt.initFrom(p);
		p->style()->drawPrimitive(QStyle::PE_Widget, &pOpt, &pp, p);
		pp.end();
		const QRgb px = pImg.pixel(8, 8);
		if (qAlpha(px) == 255) {
			bg = QColor::fromRgb(px);
			break;
		}
		if (p->autoFillBackground()) {
			bg = p->palette().color(p->backgroundRole());
			break;
		}
	}

	const int probeH = std::max(28, height());
	QImage probe(72, probeH, QImage::Format_RGBA8888_Premultiplied);
	probe.fill(bg);
	QStyleOptionSlider opt;
	initStyleOption(&opt);
	opt.rect = QRect(0, 0, probe.width(), probe.height());
	opt.orientation = Qt::Horizontal;
	opt.minimum = 0;
	opt.maximum = 1024;
	opt.sliderValue = 512;
	opt.sliderPosition = 512;
	opt.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken);
	// QStyleSheetStyle only draws ::sub-page and ::add-page when both groove and handle are active.
	opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
	{
		QPainter sp(&probe);
		style()->drawComplexControl(QStyle::CC_Slider, &opt, &sp, this);
	}
	const QRect gr = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	const QRect hr = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
	const int gy = std::clamp(gr.isValid() ? gr.center().y() : probe.height() / 2, 0, probe.height() - 1);
	const int grLeft = gr.isValid() ? gr.left() : 0;
	const int grRight = gr.isValid() ? gr.right() : (probe.width() - 1);
	const int leftSpanEnd = hr.isValid() ? hr.left() : probe.width() / 2;
	const int rightSpanStart = hr.isValid() ? (hr.right() + 1) : probe.width() / 2;
	const int leftX = std::clamp((grLeft + leftSpanEnd) / 2, 0, probe.width() - 1);
	const int rightX = std::clamp((rightSpanStart + grRight) / 2, 0, probe.width() - 1);
	const int hx = std::clamp(hr.isValid() ? hr.center().x() : probe.width() / 2, 0, probe.width() - 1);
	const int hy = std::clamp(hr.isValid() ? hr.center().y() : gy, 0, probe.height() - 1);
	const QRgb leftPx = probe.pixel(leftX, gy);
	const QRgb rightPx = probe.pixel(rightX, gy);
	const QRgb handlePx = probe.pixel(hx, hy);

	cachedBgColor = bg;
	cachedFillColor = (leftPx != bg.rgb()) ? QColor::fromRgb(leftPx)
					       : (isEnabled() ? palette().highlight().color() : palette().mid().color());
	cachedGrooveColor = (rightPx != bg.rgb()) ? QColor::fromRgb(rightPx) : palette().dark().color();
	cachedHandleColor = (handlePx != bg.rgb()) ? QColor::fromRgb(handlePx)
						   : (isEnabled() ? palette().buttonText().color() : palette().mid().color());
	colorsReady = true;
}

void AbsoluteSlider::paintEvent(QPaintEvent *event)
{
	if (lightweightMode && orientation() == Qt::Horizontal) {
		ensureLightweightColors();
		QPainter painter(this);
		const QRect clip = event ? (event->rect() & rect()) : rect();
		painter.fillRect(clip, cachedBgColor);

		const QRect groove = lightweightGrooveRect();
		const int cx = lightweightHandleCenterX(progressValue);

		const QRect grooveVisible = groove & clip;
		if (!grooveVisible.isEmpty())
			painter.fillRect(grooveVisible, cachedGrooveColor);

		const int fillWidth = std::max(0, cx - groove.left());
		if (fillWidth > 0) {
			const QRect fillRect(groove.left(), groove.top(), fillWidth, groove.height());
			const QRect fillVisible = fillRect & clip;
			if (!fillVisible.isEmpty())
				painter.fillRect(fillVisible, cachedFillColor);
		}

		const QRect handleRect(cx - kLightHandleWidth / 2, (height() - kLightHandleHeight) / 2,
				       kLightHandleWidth, kLightHandleHeight);
		if (handleRect.intersects(clip))
			painter.fillRect(handleRect & clip, cachedHandleColor);
		return;
	}

	if (!getDisplayTicks()) {
		QSlider::paintEvent(event);
		return;
	}

	QPainter painter(this);

	QStyleOptionSlider opt;
	initStyleOption(&opt);

	QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

	const bool isHorizontal = orientation() == Qt::Horizontal;

	const int sliderLength = isHorizontal ? groove.width() - handle.width() : groove.height() - handle.height();
	const int handleSize = isHorizontal ? handle.width() : handle.height();
	const int grooveSize = isHorizontal ? groove.height() : groove.width();
	const int grooveStart = isHorizontal ? groove.left() : groove.top();
	const int tickLinePos = isHorizontal ? groove.center().y() : groove.center().x();
	const int tickLength = std::max((int)(grooveSize * 1.5) + grooveSize, 8 + grooveSize);
	const int tickLineStart = tickLinePos - (tickLength / 2) + 1;

	for (double offset = minimum(); offset <= maximum(); offset += singleStep()) {
		double tickPercent = (offset - minimum()) / (maximum() - minimum());
		const int tickLineOffset = grooveStart + std::floor(sliderLength * tickPercent) + (handleSize / 2);

		const int xPos = isHorizontal ? tickLineOffset : tickLineStart;
		const int yPos = isHorizontal ? tickLineStart : tickLineOffset;

		const int tickWidth = isHorizontal ? 1 : tickLength;
		const int tickHeight = isHorizontal ? tickLength : 1;

		painter.fillRect(xPos, yPos, tickWidth, tickHeight, tickColor);
	}

	QSlider::paintEvent(event);
}
