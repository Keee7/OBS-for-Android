/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>
#include "AudioMixer.hpp"
#include "OBSAndroidDisplay.hpp"
#include "OBSBasicControls.hpp"
#include <utility/android-layout-policy.hpp>
#include <utility/android-window-fit.hpp>
#include <QDockWidget>
#include <QInputMethod>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScreen>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWindow>

void OBSBasic::ConnectAndroidScreenSignals(QScreen *scr)
{
	if (!scr || scr == androidObservedScreen)
		return;
	QObject::disconnect(androidScreenGeoConn);
	QObject::disconnect(androidScreenOrientConn);
	androidObservedScreen = scr;
	androidScreenGeoConn = connect(scr, &QScreen::availableGeometryChanged, this, [this] {
		if (androidLayoutResizeTimer)
			androidLayoutResizeTimer->start();
	});
	androidScreenOrientConn = connect(scr, &QScreen::orientationChanged, this, [this] {
		UpdateAndroidSafeAreaMargins();
		if (androidLayoutResizeTimer)
			androidLayoutResizeTimer->start();
	});
}

void OBSBasic::ConnectAndroidWindowSignals()
{
	if (QWindow *wh = windowHandle(); wh && wh != androidObservedWindow) {
		androidObservedWindow = wh;
		connect(wh, &QWindow::safeAreaMarginsChanged, this, [this] {
			UpdateAndroidSafeAreaMargins();
			if (androidLayoutResizeTimer)
				androidLayoutResizeTimer->start();
		});
		connect(wh, &QWindow::visibleChanged, this, [this](bool visible) {
			if (!visible)
				return;
			UpdateAndroidSafeAreaMargins();
			QTimer::singleShot(150, this, [this] {
				UpdateAndroidSafeAreaMargins();
				ResizeAndroidDocks();
			});
		});
		connect(wh, &QWindow::screenChanged, this, [this](QScreen *scr) {
			ConnectAndroidScreenSignals(scr);
			UpdateAndroidSafeAreaMargins();
			if (androidLayoutResizeTimer)
				androidLayoutResizeTimer->start();
		});
	}
}

void OBSBasic::UpdateAndroidSafeAreaMargins()
{
	ConnectAndroidWindowSignals();
	// Wait until the native QtWindow exists so ObsWindowInsetsHost subtracts the
	// QtWindow's own offset inside DecorView instead of applying raw root insets twice.
	QWindow *wh = windowHandle();
	if (!wh || !wh->handle())
		return;
	const QMargins insets = obsAndroidWindowSafeMargins(this);
	if (contentsMargins() != insets) {
		setContentsMargins(insets);
		obsAndroidDisplayLog("Android safe area margins: l=%d t=%d r=%d b=%d", insets.left(), insets.top(),
				     insets.right(), insets.bottom());
	}
}

void OBSBasic::InitAndroidResponsiveLayout()
{
	setDockOptions(dockOptions() & ~QMainWindow::AnimatedDocks);
	setStyleSheet(styleSheet() + QStringLiteral("\nOBSBasic::separator { width: 10px; height: 10px; }"));
	androidLayoutResizeTimer = new QTimer(this);
	androidLayoutResizeTimer->setSingleShot(true);
	androidLayoutResizeTimer->setInterval(120);
	connect(androidLayoutResizeTimer, &QTimer::timeout, this, &OBSBasic::ResizeAndroidDocks);
	ConnectAndroidScreenSignals(screen());
	androidMainDocks = {ui->sourcesDock, ui->scenesDock, ui->mixerDock, ui->transitionsDock, controlsDock};
	for (QDockWidget *dock : androidMainDocks) {
		dock->setMinimumSize(0, 0);
		if (dock->isFloating())
			dock->setFloating(false);
		dock->installEventFilter(this);
	}
	connect(controlsDock, &QDockWidget::dockLocationChanged, this, [this] {
		androidLayoutResizeTimer->start();
	});
	connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, this, [this] {
		if (!QGuiApplication::inputMethod()->isVisible())
			androidLayoutResizeTimer->start();
	});
	ensurePolished();
	UpdateAndroidSafeAreaMargins();
	ResizeAndroidDocks();
}

void OBSBasic::resizeEvent(QResizeEvent *event)
{
	OBSMainWindow::resizeEvent(event);
	// Coalesce live freeform resizing. Nothing runs periodically or per frame.
	if (androidLayoutResizeTimer)
		androidLayoutResizeTimer->start();
}

void OBSBasic::SetAndroidPhoneDockLayout(bool phone)
{
	CancelAndroidDockDrag();
	// Trim vertical padding to leave room for the mixer and status bar.
	const QString compactInputs = phone ? QStringLiteral(
		"QPushButton, QComboBox, QSpinBox { padding-top: 2px; padding-bottom: 2px; }") : QString();
	ui->transitionsFrame->setStyleSheet(compactInputs);
	controlsDock->widget()->setStyleSheet(compactInputs);
	ui->statusbar->SetAndroidPhoneLayout(phone);
	// The source name can shrink on phones while its action buttons remain reachable.
	ui->contextSourceLabel->setMinimumWidth(phone ? 48 : ui->contextSourceLabel->maximumWidth());
	// Put the transition and its duration on one row to keep the mixer meter visible.
	if (phone) {
		ui->verticalLayout_8->removeWidget(ui->transitions);
		ui->horizontalLayout_3->insertWidget(0, ui->transitions, 1);
	} else {
		ui->horizontalLayout_3->removeWidget(ui->transitions);
		ui->verticalLayout_8->insertWidget(0, ui->transitions);
	}

	if (auto *mixer = qobject_cast<AudioMixer *>(ui->mixerDock->widget()))
		mixer->setPhoneLayout(phone);

	if (!phone) {
		restoreState(androidRegularDockLayout);
		androidRegularDockLayout.clear();
		on_lockDocks_toggled(ui->lockDocks->isChecked());
		return;
	}

	androidRegularDockLayout = saveState();
	if (androidPhoneDocks.isEmpty())
		androidPhoneDocks = androidMainDocks;
	ApplyAndroidPhoneDockOrder();
}

void OBSBasic::ApplyAndroidPhoneDockOrder(QDockWidget *rightTab)
{
	QList<bool> shown;
	for (QDockWidget *dock : androidPhoneDocks) {
		shown.append(!dock->isHidden());
		removeDockWidget(dock);
	}

	setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::South);
	setTabPosition(Qt::RightDockWidgetArea, QTabWidget::South);

	addDockWidget(Qt::LeftDockWidgetArea, androidPhoneDocks[0]);
	splitDockWidget(androidPhoneDocks[0], androidPhoneDocks[1], Qt::Vertical);
	addDockWidget(Qt::RightDockWidgetArea, androidPhoneDocks[2]);
	splitDockWidget(androidPhoneDocks[2], androidPhoneDocks[4], Qt::Vertical);
	tabifyDockWidget(androidPhoneDocks[2], androidPhoneDocks[3]);

	for (qsizetype i = 0; i < androidPhoneDocks.size(); ++i)
		androidPhoneDocks[i]->setVisible(shown[i]);
	if (!rightTab)
		rightTab = androidPhoneDocks[2];
	if (!rightTab->isHidden())
		rightTab->raise();
}

QDockWidget *OBSBasic::AndroidPhoneRightTab() const
{
	QDockWidget *fallback = nullptr;
	for (auto *dock : {androidPhoneDocks[2], androidPhoneDocks[3]}) {
		if (dock->isHidden())
			continue;
		if (!dock->visibleRegion().isEmpty())
			return dock;
		if (!fallback)
			fallback = dock;
	}
	return fallback;
}

void OBSBasic::ResizeAndroidPhoneDockColumns(int leftWidth, int rightWidth, int leftTopHeight, int leftBottomHeight,
					     int rightTopHeight, int rightBottomHeight)
{
	QDockWidget *leftAnchor = !androidPhoneDocks[0]->isHidden() ? androidPhoneDocks[0] : androidPhoneDocks[1];
	QDockWidget *rightTop = AndroidPhoneRightTab();
	QDockWidget *rightAnchor = rightTop ? rightTop : androidPhoneDocks[4];

	QList<QDockWidget *> hDocks;
	QList<int> hWidths;
	if (!leftAnchor->isHidden()) {
		hDocks.append(leftAnchor);
		hWidths.append(leftWidth);
	}
	if (!rightAnchor->isHidden()) {
		hDocks.append(rightAnchor);
		hWidths.append(rightWidth);
	}
	if (!hDocks.isEmpty())
		resizeDocks(hDocks, hWidths, Qt::Horizontal);
	if (!androidPhoneDocks[0]->isHidden() && !androidPhoneDocks[1]->isHidden())
		resizeDocks(androidPhoneDocks.mid(0, 2), {leftTopHeight, leftBottomHeight}, Qt::Vertical);
	if (rightTop && !androidPhoneDocks[4]->isHidden())
		resizeDocks({rightTop, androidPhoneDocks[4]}, {rightTopHeight, rightBottomHeight}, Qt::Vertical);
}

void OBSBasic::CancelAndroidDockDrag()
{
	androidDraggedDock.clear();
	if (androidDockDropIndicator)
		androidDockDropIndicator->hide();
}

void OBSBasic::UpdateAndroidControlColumns(bool phone)
{
	const Qt::DockWidgetArea area = dockWidgetArea(controlsDock);
	const bool side = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
	if (auto *controls = qobject_cast<OBSBasicControls *>(controlsDock->widget()))
		controls->setAndroidColumnCount(phone || !side ? 2 : 1);
}

bool OBSBasic::eventFilter(QObject *watched, QEvent *event)
{
	const auto type = event->type();
	if (androidDraggedDock && watched == androidDraggedDock &&
	    (type == QEvent::Hide || type == QEvent::UngrabMouse))
		CancelAndroidDockDrag();
	if (ui->lockDocks->isChecked() ||
	    (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease))
		return OBSMainWindow::eventFilter(watched, event);

	auto *dock = qobject_cast<QDockWidget *>(watched);
	const auto *mouse = static_cast<QMouseEvent *>(event);
	const QPoint screenPoint = mouse->globalPosition().toPoint();
	if (type == QEvent::MouseButtonPress) {
		if (mouse->button() == Qt::LeftButton && androidMainDocks.contains(dock) && !dock->isFloating() &&
		    dock->widget() &&
		    mouse->position().y() < dock->widget()->geometry().top()) {
			androidDraggedDock = dock;
			androidDockDragStart = screenPoint;
			return true;
		}
		return OBSMainWindow::eventFilter(watched, event);
	}
	if (!androidDraggedDock || dock != androidDraggedDock)
		return OBSMainWindow::eventFilter(watched, event);

	QDockWidget *target = nullptr;
	const bool phone = !androidRegularDockLayout.isEmpty();
	const QRect viewport = contentsRect();
	const QPoint point = mapFromGlobal(screenPoint);
	const bool moved = (screenPoint - androidDockDragStart).manhattanLength() >= QApplication::startDragDistance();
	if (moved) {
		for (auto *candidate : androidMainDocks) {
			if (candidate != dock && candidate->isVisible() && !candidate->isFloating() &&
			    (!phone || !candidate->visibleRegion().isEmpty()) &&
			    candidate->geometry().contains(point) && dock->isAreaAllowed(dockWidgetArea(candidate))) {
				target = candidate;
				break;
			}
		}
	}
	auto area = target ? dockWidgetArea(target) : Qt::NoDockWidgetArea;
	if (!phone && moved && !target && viewport.contains(point)) {
		// Keep empty dock areas reachable. Show the insertion edge without
		// opening a floating window over the canvas.
		constexpr int edge = 28;
		if (point.x() - viewport.left() < edge)
			area = Qt::LeftDockWidgetArea;
		else if (viewport.right() - point.x() < edge)
			area = Qt::RightDockWidgetArea;
		else if (point.y() - viewport.top() < edge)
			area = Qt::TopDockWidgetArea;
		else if (viewport.bottom() - point.y() < edge)
			area = Qt::BottomDockWidgetArea;
		if (area == dockWidgetArea(dock) || !dock->isAreaAllowed(area))
			area = Qt::NoDockWidgetArea;
	}
	const bool vertical = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;

	if (type == QEvent::MouseMove) {
		if (area != Qt::NoDockWidgetArea) {
			if (!androidDockDropIndicator)
				androidDockDropIndicator = new QRubberBand(QRubberBand::Rectangle, this);
			QRect indicator = target ? target->geometry() : viewport;
			if (!target) {
				switch (area) {
				case Qt::LeftDockWidgetArea:
					indicator.setWidth(6);
					break;
				case Qt::RightDockWidgetArea:
					indicator.setLeft(indicator.right() - 5);
					break;
				case Qt::TopDockWidgetArea:
					indicator.setHeight(6);
					break;
				case Qt::BottomDockWidgetArea:
					indicator.setTop(indicator.bottom() - 5);
					break;
				default:
					break;
				}
			} else if (!phone) {
				if (vertical)
					indicator.setTop(indicator.bottom() - 5);
				else if (isRightToLeft())
					indicator.setWidth(6);
				else
					indicator.setLeft(indicator.right() - 5);
			}
			if (androidDockDropIndicator->geometry() != indicator)
				androidDockDropIndicator->setGeometry(indicator);
			if (androidDockDropIndicator->isHidden()) {
				androidDockDropIndicator->show();
				androidDockDropIndicator->raise();
			}
		} else if (androidDockDropIndicator) {
			androidDockDropIndicator->hide();
		}
		return true;
	}
	if (mouse->button() != Qt::LeftButton)
		return false;

	CancelAndroidDockDrag();
	if (area == Qt::NoDockWidgetArea)
		return true;

	if (phone) {
		QDockWidget *oldLeft = !androidPhoneDocks[0]->isHidden() ? androidPhoneDocks[0] : androidPhoneDocks[1];
		QDockWidget *oldRightTop = AndroidPhoneRightTab();
		QDockWidget *oldRight = oldRightTop ? oldRightTop : androidPhoneDocks[4];
		const int leftWidth = oldLeft->width();
		const int rightWidth = oldRight->width();
		const int leftTopHeight = androidPhoneDocks[0]->height();
		const int leftBottomHeight = androidPhoneDocks[1]->height();
		const int rightTopHeight = oldRightTop ? oldRightTop->height() : androidPhoneDocks[2]->height();
		const int rightBottomHeight = androidPhoneDocks[4]->height();
		androidPhoneDocks.swapItemsAt(androidPhoneDocks.indexOf(dock), androidPhoneDocks.indexOf(target));
		QDockWidget *rightTab = oldRightTop;
		if (dock == androidPhoneDocks[2] || dock == androidPhoneDocks[3])
			rightTab = dock;
		else if (target == androidPhoneDocks[2] || target == androidPhoneDocks[3])
			rightTab = target;
		ApplyAndroidPhoneDockOrder(rightTab);
		ResizeAndroidPhoneDockColumns(leftWidth, rightWidth, leftTopHeight, leftBottomHeight, rightTopHeight,
					      rightBottomHeight);
	} else {
		const QByteArray previousLayout = saveState();
		removeDockWidget(dock);
		const auto direction = vertical ? Qt::Vertical : Qt::Horizontal;
		if (target)
			splitDockWidget(target, dock, direction);
		else
			addDockWidget(area, dock, direction);
		dock->show();
		UpdateAndroidControlColumns(false);
		const QSize minimum = layout()->minimumSize();
		if (minimum.width() > viewport.width() || minimum.height() > viewport.height()) {
			restoreState(previousLayout);
			UpdateAndroidControlColumns(false);
		}
	}
	return true;
}

void OBSBasic::ResizeAndroidDocks()
{
	if (QGuiApplication::inputMethod()->isVisible())
		return;

	// Qt Android reports the Activity's root layout through availableGeometry.
	// QWidget::size() can change when a dock is dragged or its minimum size grows.
	const auto *display = screen();
	const QSize available = display ? display->availableGeometry().size() : size();
	if (available.isEmpty() || androidMainDocks.isEmpty())
		return;
	if (size() != available)
		resize(available);
	UpdateAndroidSafeAreaMargins();
	const QSize contentSize = contentsRect().size().isEmpty() ? available : contentsRect().size();
	const bool wasPhone = !androidRegularDockLayout.isEmpty();
	const bool phone = OBSAndroidLayout::phoneLandscape(available.width(), available.height(), wasPhone);
	if (phone != wasPhone) {
		SetAndroidPhoneDockLayout(phone);
		if (IsPreviewProgramMode())
			ResetUI();
	}

	UpdateAndroidControlColumns(phone);

	// Preserve user-set divider positions and dock arrangements between real window resizes.
	// Only populate the skip-cache once the window is visible with polished metrics and real insets.
	if (isVisible()) {
		if (contentSize == androidDockViewport && contentsMargins() == androidDockMargins)
			return;
		androidDockViewport = contentSize;
		androidDockMargins = contentsMargins();
	}

	const QVariant previousMode = property("androidCompactLayout");
	const bool compact = OBSAndroidLayout::compact(available.width(), available.height(), previousMode.toBool());
	if (!previousMode.isValid() || compact != previousMode.toBool()) {
		setProperty("androidCompactLayout", compact);
		ui->scenes->setProperty("androidCompactLayout", compact);
		ui->sources->setProperty("androidCompactLayout", compact);
		const int rowHeight = OBSAndroidLayout::touchRowHeight(compact);
		if (!ui->scenes->GetGridMode())
			ui->scenes->setStyleSheet(QStringLiteral("QListView::item { min-height: %1px; padding-top: 0px; padding-bottom: 0px; }")
						 .arg(rowHeight));
		ui->scenes->doItemsLayout();
		ui->sources->doItemsLayout();

		const QSize buttonSize = compact ? QSize(36, 36) : QSize(48, 48);
		const QSize iconSize = compact ? QSize(16, 16) : QSize(24, 24);
		const QSize transitionBtnSize = compact ? QSize(36, 36) : QSize(40, 40);
		for (QPushButton *btn : {ui->transitionAdd, ui->transitionRemove, ui->transitionProps}) {
			if (!btn)
				continue;
			btn->setFixedSize(transitionBtnSize);
		}
		for (QToolBar *bar : {ui->scenesToolbar, ui->sourcesToolbar}) {
			bar->setIconSize(iconSize);
			for (QAction *action : bar->actions()) {
				if (!action->isSeparator()) {
					if (auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(action)))
						button->setProperty("androidTouchAction", true);
				}
			}
			bar->setStyleSheet(QStringLiteral(
				"QToolBar { min-height: %1px; max-height: %1px; padding: 1px; }"
				"QToolButton[androidTouchAction=\"true\"] { min-width: %2px; max-width: %2px;"
				" min-height: %3px; max-height: %3px; padding: 0px; margin: 0px; border: none; }")
				.arg(buttonSize.height() + 2).arg(buttonSize.width()).arg(buttonSize.height()));
		}
		UpdateContextBar(true);
		obsAndroidDisplayLog("Android responsive mode: compact=%d viewport=%dx%d row=%d", int(compact),
				     contentSize.width(), contentSize.height(), rowHeight);
	}
	/* Keep all five panels visible. Reserve room for the complete touch
	 * toolbars, then share the remaining width between the bottom panels. */
	const int toolbarWidth = qMax(ui->scenesToolbar->sizeHint().width(), ui->sourcesToolbar->sizeHint().width());

	if (phone) {
		const int availableWidth = contentSize.width();
		const int minToolbarColumn = toolbarWidth + 8;
		const auto toolbarOnSide = [this](Qt::DockWidgetArea area) {
			return (!ui->scenesDock->isHidden() && dockWidgetArea(ui->scenesDock) == area) ||
			       (!ui->sourcesDock->isHidden() && dockWidgetArea(ui->sourcesDock) == area);
		};
		const int left = qMax(toolbarOnSide(Qt::LeftDockWidgetArea) ? minToolbarColumn : 0,
				      availableWidth * 25 / 100);
		const int right = qMax(toolbarOnSide(Qt::RightDockWidgetArea) ? minToolbarColumn : 0,
				       availableWidth * 30 / 100);
		const int statusHeight = ui->statusbar->isHidden() ? 0 : ui->statusbar->sizeHint().height();
		const int availableHeight = qMax(1, contentSize.height() - statusHeight);
		const int half = availableHeight / 2;
		const int controls = availableHeight * 43 / 100;
		ResizeAndroidPhoneDockColumns(left, right, half, availableHeight - half,
					      availableHeight - controls, controls);
		obsAndroidDisplayLog("Android phone landscape layout: viewport=%dx%d content=%dx%d left=%d right=%d status=%d",
				     available.width(), available.height(), contentSize.width(), contentSize.height(),
				     left, right, statusHeight);
		return;
	}

	const QSize win = contentsRect().size();

	QList<QDockWidget *> leftDocks, rightDocks, topDocks, bottomDocks;
	for (QDockWidget *dock :
	     {ui->scenesDock, ui->sourcesDock, ui->mixerDock, ui->transitionsDock, controlsDock.data()}) {
		if (!dock || dock->isHidden() || dock->isFloating())
			continue;
		switch (dockWidgetArea(dock)) {
		case Qt::LeftDockWidgetArea:
			leftDocks.append(dock);
			break;
		case Qt::RightDockWidgetArea:
			rightDocks.append(dock);
			break;
		case Qt::TopDockWidgetArea:
			topDocks.append(dock);
			break;
		case Qt::BottomDockWidgetArea:
			bottomDocks.append(dock);
			break;
		default:
			break;
		}
	}

	const bool leftHasToolbar = leftDocks.contains(ui->scenesDock) || leftDocks.contains(ui->sourcesDock);
	const bool rightHasToolbar = rightDocks.contains(ui->scenesDock) || rightDocks.contains(ui->sourcesDock);
	int leftW = 0;
	int rightW = 0;

	if (!leftDocks.isEmpty() && !rightDocks.isEmpty()) {
		leftW = qMax(leftHasToolbar ? toolbarWidth + 8 : 0, qMin(win.width() * 26 / 100, 260));
		rightW = qMax(rightHasToolbar ? toolbarWidth + 8 : 0, qMin(win.width() * 24 / 100, 240));
		const int maxTotalSide = qMax(2, win.width() * 52 / 100);
		if (leftW + rightW > maxTotalSide) {
			if (leftHasToolbar && !rightHasToolbar) {
				leftW = qMin(leftW, maxTotalSide);
				rightW = qMax(140, maxTotalSide - leftW);
			} else if (rightHasToolbar && !leftHasToolbar) {
				rightW = qMin(rightW, maxTotalSide);
				leftW = qMax(140, maxTotalSide - rightW);
			} else {
				leftW = maxTotalSide / 2;
				rightW = maxTotalSide - leftW;
			}
		}
	} else if (!leftDocks.isEmpty()) {
		leftW = qMax(leftHasToolbar ? toolbarWidth + 8 : 0, qMin(win.width() * 28 / 100, 280));
	} else if (!rightDocks.isEmpty()) {
		rightW = qMax(rightHasToolbar ? toolbarWidth + 8 : 0, qMin(win.width() * 28 / 100, 280));
	}

	if (!leftDocks.isEmpty()) {
		QList<int> widths(leftDocks.size(), leftW);
		resizeDocks(leftDocks, widths, Qt::Horizontal);
	}
	if (!rightDocks.isEmpty()) {
		QList<int> widths(rightDocks.size(), rightW);
		resizeDocks(rightDocks, widths, Qt::Horizontal);
	}

	const int restW = qMax(1, win.width() - leftW - rightW);
	const bool haveTop = !topDocks.isEmpty();
	const bool haveBottom = !bottomDocks.isEmpty();
	const int maxAreaH =
		(haveTop && haveBottom) ? qMax(96, win.height() * 26 / 100) : qMax(120, win.height() * 36 / 100);

	auto sizeHorizontalArea = [&](const QList<QDockWidget *> &docks) -> int {
		if (docks.isEmpty())
			return 0;
		int totalWeight = 0;
		QList<int> weights;
		weights.reserve(docks.size());
		for (QDockWidget *d : docks) {
			int w = 30;
			if (d == ui->mixerDock)
				w = 35;
			else if (d == ui->transitionsDock)
				w = 25;
			else if (d == controlsDock)
				w = 40;
			weights.append(w);
			totalWeight += w;
		}
		QList<int> horizSizes;
		horizSizes.reserve(docks.size());
		int assigned = 0;
		for (qsizetype i = 0; i < docks.size(); ++i) {
			const int s = (i + 1 == docks.size()) ? qMax(1, restW - assigned)
							      : qMax(1, restW * weights[i] / qMax(1, totalWeight));
			horizSizes.append(s);
			assigned += s;
		}
		resizeDocks(docks, horizSizes, Qt::Horizontal);

		int desiredH = 0;
		for (QDockWidget *d : docks) {
			const auto hint = d == controlsDock ? d->sizeHint() : d->minimumSizeHint();
			desiredH = qMax(desiredH, hint.height());
		}
		const int rowH = qBound(80, desiredH, maxAreaH);
		QList<int> vertSizes(docks.size(), rowH);
		resizeDocks(docks, vertSizes, Qt::Vertical);
		return rowH;
	};

	const int topH = sizeHorizontalArea(topDocks);
	const int bottomH = sizeHorizontalArea(bottomDocks);
	obsAndroidDisplayLog("Android layout: win=%dx%d left=%d right=%d top=%d bottom=%d preview=%dx%d", win.width(),
			     win.height(), leftW, rightW, topH, bottomH, ui->preview->width(), ui->preview->height());
}
