/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#ifdef __ANDROID__
#include "OBSAndroidDisplay.hpp"
#include <utility/android-layout-policy.hpp>
#include <QResizeEvent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

void OBSBasic::InitAndroidResponsiveLayout()
{
	androidLayoutResizeTimer = new QTimer(this);
	androidLayoutResizeTimer->setSingleShot(true);
	androidLayoutResizeTimer->setInterval(120);
	connect(androidLayoutResizeTimer, &QTimer::timeout, this, &OBSBasic::ResizeAndroidDocks);
	QTimer::singleShot(0, this, &OBSBasic::ResizeAndroidDocks);
}

void OBSBasic::resizeEvent(QResizeEvent *event)
{
	OBSMainWindow::resizeEvent(event);
	// Coalesce live freeform resizing. Nothing runs periodically or per frame.
	if (androidLayoutResizeTimer)
		androidLayoutResizeTimer->start();
}

void OBSBasic::ResizeAndroidDocks()
{
	const QVariant previousMode = property("androidCompactLayout");
	const bool compact = OBSAndroidLayout::compact(width(), height(), previousMode.toBool());
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

		const QSize preferred = ui->transitionAdd->sizeHint();
		const QSize buttonSize = compact ? preferred : preferred.expandedTo(QSize(48, 48));
		const QSize iconSize = compact ? ui->transitionAdd->iconSize() : QSize(24, 24);
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
		obsAndroidDisplayLog("Android responsive mode: compact=%d window=%dx%d row=%d", int(compact), width(), height(), rowHeight);
	}
	/* Keep all five panels visible. Reserve room for the complete touch
	 * toolbars, then share the remaining width between the bottom panels. */
	const int toolbarWidth = qMax(ui->scenesToolbar->sizeHint().width(), ui->sourcesToolbar->sizeHint().width());

	/* 左栏宽度：下界保证工具栏的 5 个按钮放得下，上界防止在宽屏上无限膨胀。
	 *
	 * 上界 280 沿用上游 on_resetDocks_triggered 的写法（那里是
	 * min(width*30%, 280)）。没有它的话，1280 宽的平板上这里是 358、
	 * 1600 宽是 448 —— 左栏随屏幕变宽一直涨，把预览挤掉。
	 *
	 * 手写 qMax/qMin 而不是 qBound：下界可能大于上界（工具栏特别宽时），
	 * 那种情况应该保工具栏完整、让上限失效，而 qBound 要求 min <= max。 */
	const int side = qMax(toolbarWidth + 8, qMin(width() * 28 / 100, 280));
	const int rest = qMax(1, width() - side);
	resizeDocks({ui->scenesDock, ui->sourcesDock}, {side, side}, Qt::Horizontal);
	resizeDocks({ui->mixerDock, ui->transitionsDock, controlsDock},
		    {rest * 35 / 100, rest * 25 / 100, rest * 40 / 100}, Qt::Horizontal);

	/* Give spare height to the preview without overriding content minima. */
	const int bottom = qMax(ui->mixerDock->minimumSizeHint().height(),
				qMax(ui->transitionsDock->minimumSizeHint().height(),
				     controlsDock->minimumSizeHint().height()));
	resizeDocks({ui->mixerDock, ui->transitionsDock, controlsDock}, {bottom, bottom, bottom}, Qt::Vertical);
	obsAndroidDisplayLog("Android layout: side=%d bottom=%d preview=%dx%d", side, bottom,
			     ui->preview->width(), ui->preview->height());
}
#endif

void setupDockAction(QDockWidget *dock)
{
	QAction *action = dock->toggleViewAction();

	auto neverDisable = [action]() {
		QSignalBlocker block(action);
		action->setEnabled(true);
	};

	auto newToggleView = [dock](bool check) {
		QSignalBlocker block(dock);
		dock->setVisible(check);
	};

	// Replace the slot connected by default
	QObject::disconnect(action, &QAction::triggered, nullptr, 0);
	QObject::connect(action, &QAction::triggered, dock, newToggleView);

	// Make the action unable to be disabled
	QObject::connect(action, &QAction::enabledChanged, action, neverDisable);
}

void OBSBasic::on_resetDocks_triggered(bool force)
{
#ifdef BROWSER_AVAILABLE
	if ((extraDocks.size() || extraCustomDocks.size() || extraBrowserDocks.size()) && !force)
#else
	if ((extraDocks.size() || extraCustomDocks.size()) && !force)
#endif
	{
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("ResetUIWarning.Title"), QTStr("ResetUIWarning.Text"));

		if (button == QMessageBox::No) {
			return;
		}
	}

#define RESET_DOCKLIST(dockList)                                                                               \
	for (int i = dockList.size() - 1; i >= 0; i--) {                                                       \
		dockList[i]->setVisible(true);                                                                 \
		dockList[i]->setFloating(true);                                                                \
		dockList[i]->move(frameGeometry().topLeft() + rect().center() - dockList[i]->rect().center()); \
		dockList[i]->setVisible(false);                                                                \
	}

	RESET_DOCKLIST(extraDocks)
	RESET_DOCKLIST(extraCustomDocks)
#ifdef BROWSER_AVAILABLE
	RESET_DOCKLIST(extraBrowserDocks)
#endif
#undef RESET_DOCKLIST

	restoreState(startingDockLayout);
	ui->sideDocks->setChecked(true);

	int cx = width();
	int bottomDocksHeight = height();

	bottomDocksHeight = bottomDocksHeight * 225 / 1000;

	ui->scenesDock->setVisible(true);
	ui->sourcesDock->setVisible(true);
	ui->mixerDock->setVisible(true);
	ui->transitionsDock->setVisible(true);
	controlsDock->setVisible(true);
	statsDock->setVisible(false);
	statsDock->setFloating(true);

	QList<QDockWidget *> bottomDocks{ui->mixerDock, ui->transitionsDock, controlsDock};

	resizeDocks(bottomDocks, {bottomDocksHeight, bottomDocksHeight, bottomDocksHeight}, Qt::Vertical);
	resizeDocks(bottomDocks, {cx * 45 / 100, cx * 14 / 100, cx * 16 / 100}, Qt::Horizontal);

	int sideDockWidth = std::min(width() * 30 / 100, 280);
	resizeDocks({ui->scenesDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);

#ifdef __ANDROID__
	QTimer::singleShot(0, this, &OBSBasic::ResizeAndroidDocks);
#endif

	activateWindow();
}

void OBSBasic::on_lockDocks_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

#ifdef __ANDROID__
	/* 渲染期把 dock 浮成独立原生窗口会销毁当前 Surface：makeCurrent() 拿不到
	 * EGLSurface → QRhiGles2 初始化失败 → SIGSEGV（PORTING.md「浮动 dock 崩溃」），
	 * 小屏上浮动后也找不回。仅去掉 Floatable，Movable/Closable 按上游原值保留；
	 * 桌面不受影响。放在 mainFeatures 派生之前，几个主 dock 一并生效。 */
	features &= ~QDockWidget::DockWidgetFloatable;
#endif

	QDockWidget::DockWidgetFeatures mainFeatures = features;
	mainFeatures &= ~QDockWidget::QDockWidget::DockWidgetClosable;

	ui->scenesDock->setFeatures(mainFeatures);
	ui->sourcesDock->setFeatures(mainFeatures);
	ui->mixerDock->setFeatures(mainFeatures);
	ui->transitionsDock->setFeatures(mainFeatures);
	controlsDock->setFeatures(mainFeatures);
	statsDock->setFeatures(features);

	for (int i = extraDocks.size() - 1; i >= 0; i--) {
		extraDocks[i]->setFeatures(features);
	}

	for (int i = extraCustomDocks.size() - 1; i >= 0; i--) {
		extraCustomDocks[i]->setFeatures(features);
	}

#ifdef BROWSER_AVAILABLE
	for (int i = extraBrowserDocks.size() - 1; i >= 0; i--) {
		extraBrowserDocks[i]->setFeatures(features);
	}
#endif
}

void OBSBasic::on_sideDocks_toggled(bool side)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", side);

	setDockCornersVertical(side);
}

void OBSBasic::AddDockWidget(QDockWidget *dock, Qt::DockWidgetArea area, bool extraBrowser)
{
	if (dock->objectName().isEmpty()) {
		return;
	}

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

#ifdef __ANDROID__
	/* 同上（PORTING.md「浮动 dock 崩溃」）：Android 上不给新增 dock 浮动能力。 */
	features &= ~QDockWidget::DockWidgetFloatable;
#endif

	setupDockAction(dock);
	dock->setFeatures(features);
	addDockWidget(area, dock);

#ifdef BROWSER_AVAILABLE
	if (extraBrowser && extraBrowserMenuDocksSeparator.isNull()) {
		extraBrowserMenuDocksSeparator = ui->menuDocks->addSeparator();
	}

	if (!extraBrowser && !extraBrowserMenuDocksSeparator.isNull()) {
		ui->menuDocks->insertAction(extraBrowserMenuDocksSeparator, dock->toggleViewAction());
	} else {
		ui->menuDocks->addAction(dock->toggleViewAction());
	}

	if (extraBrowser) {
		return;
	}
#else
	UNUSED_PARAMETER(extraBrowser);

	ui->menuDocks->addAction(dock->toggleViewAction());
#endif

	extraDockNames.push_back(dock->objectName());
	extraDocks.push_back(std::shared_ptr<QDockWidget>(dock));
}

void OBSBasic::RemoveDockWidget(const QString &name)
{
	if (extraDockNames.contains(name)) {
		int idx = extraDockNames.indexOf(name);
		extraDockNames.removeAt(idx);
		extraDocks[idx].reset();
		extraDocks.removeAt(idx);
	} else if (extraCustomDockNames.contains(name)) {
		int idx = extraCustomDockNames.indexOf(name);
		extraCustomDockNames.removeAt(idx);
		removeDockWidget(extraCustomDocks[idx]);
		extraCustomDocks.removeAt(idx);
	}
}

bool OBSBasic::IsDockObjectNameUsed(const QString &name)
{
	QStringList list;
	list << "scenesDock"
	     << "sourcesDock"
	     << "mixerDock"
	     << "transitionsDock"
	     << "controlsDock"
	     << "statsDock";
	list << extraDockNames;
	list << extraCustomDockNames;

	return list.contains(name);
}

void OBSBasic::AddCustomDockWidget(QDockWidget *dock)
{
	// Prevent the object name from being changed
	connect(dock, &QObject::objectNameChanged, this, &OBSBasic::RepairCustomExtraDockName);

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

#ifdef __ANDROID__
	/* 同上（PORTING.md「浮动 dock 崩溃」）：Android 上不给新增 dock 浮动能力。 */
	features &= ~QDockWidget::DockWidgetFloatable;
#endif

	dock->setFeatures(features);
	addDockWidget(Qt::RightDockWidgetArea, dock);

	extraCustomDockNames.push_back(dock->objectName());
	extraCustomDocks.push_back(dock);
}

void OBSBasic::setDockCornersVertical(bool vertical)
{
	if (vertical) {
		setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	} else {
		setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	}
}

void OBSBasic::RepairCustomExtraDockName()
{
	QDockWidget *dock = reinterpret_cast<QDockWidget *>(sender());
	int idx = extraCustomDocks.indexOf(dock);
	QSignalBlocker block(dock);

	if (idx == -1) {
		blog(LOG_WARNING, "A custom dock got its object name changed");
		return;
	}

	blog(LOG_WARNING, "The custom dock '%s' got its object name restored", QT_TO_UTF8(extraCustomDockNames[idx]));

	dock->setObjectName(extraCustomDockNames[idx]);
}
