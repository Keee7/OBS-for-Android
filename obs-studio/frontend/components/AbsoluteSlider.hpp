#pragma once

#include <slider-ignorewheel.hpp>

class AbsoluteSlider : public SliderIgnoreScroll {
	Q_OBJECT
	Q_PROPERTY(QColor tickColor READ getTickColor WRITE setTickColor DESIGNABLE true)

public:
	AbsoluteSlider(QWidget *parent = nullptr);
	AbsoluteSlider(Qt::Orientation orientation, QWidget *parent = nullptr);

	bool getDisplayTicks() const;
	void setDisplayTicks(bool display);

	QColor getTickColor() const;
	void setTickColor(QColor c);

	void setLightweightMode(bool enable);
	bool isLightweightMode() const { return lightweightMode; }
	void setProgressValue(int val);
	int effectiveValue() const { return lightweightMode ? progressValue : value(); }

signals:
	void absoluteSliderHovered(int value);

protected:
	virtual void mouseMoveEvent(QMouseEvent *event) override;
	virtual void mousePressEvent(QMouseEvent *event) override;
	virtual void mouseReleaseEvent(QMouseEvent *event) override;
	virtual bool eventFilter(QObject *obj, QEvent *event) override;
	virtual void changeEvent(QEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;

	int posToRangeValue(QMouseEvent *event);

	virtual void paintEvent(QPaintEvent *event) override;

private:
	void ensureLightweightColors();
	QRect lightweightGrooveRect() const;
	int lightweightHandleCenterX(int val) const;
	QRect lightweightDirtyRect(int oldVal, int newVal) const;

	bool dragging = false;
	bool displayTicks = false;
	bool lightweightMode = false;
	bool colorsReady = false;
	int progressValue = 0;

	QColor tickColor;
	QColor cachedBgColor;
	QColor cachedGrooveColor;
	QColor cachedFillColor;
	QColor cachedHandleColor;
};
