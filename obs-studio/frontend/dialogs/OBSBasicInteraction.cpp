/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include "OBSBasicInteraction.hpp"

#include <dialogs/OBSBasicProperties.hpp>
#include <utility/OBSEventFilter.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QInputEvent>
#ifdef __ANDROID__
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTouchEvent>
#include <cmath>
#include <cstring>
#include <utility/android-window-fit.hpp>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <Windows.h>
#endif

#include "moc_OBSBasicInteraction.cpp"

using namespace std;

OBSBasicInteraction::OBSBasicInteraction(QWidget *parent, OBSSource source_)
	: QDialog(parent),
	  main(qobject_cast<OBSBasic *>(parent)),
	  ui(new Ui::OBSBasicInteraction),
	  source(source_),
	  removedSignal(obs_source_get_signal_handler(source), "remove", OBSBasicInteraction::SourceRemoved, this),
	  renamedSignal(obs_source_get_signal_handler(source), "rename", OBSBasicInteraction::SourceRenamed, this),
	  eventFilter(BuildEventFilter())
{
	int cx = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cx");
	int cy = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cy");

	Qt::WindowFlags flags = windowFlags();
	Qt::WindowFlags helpFlag = Qt::WindowContextHelpButtonHint;
	setWindowFlags(flags & (~helpFlag));

	ui->setupUi(this);

#ifdef __ANDROID__
	auto *headerLayout = new QHBoxLayout;
	headerLayout->setContentsMargins(0, 0, 0, 4);
	headerLayout->setSpacing(8);
	auto *titleLabel = new QLabel(this);
	titleLabel->setMinimumWidth(0);
	titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	connect(this, &QWidget::windowTitleChanged, titleLabel, &QLabel::setText);
	headerLayout->addWidget(titleLabel, 1);

	const char *sourceId = obs_source_get_id(source);
	if (sourceId && strcmp(sourceId, "android_browser_source") == 0) {
		auto *backButton = new QPushButton(QTStr("Basic.InteractionWindow.Back"), this);
		backButton->setObjectName(QStringLiteral("androidInteractionBackButton"));
		backButton->setMinimumSize(64, 36);
		backButton->setAutoDefault(false);
		backButton->setDefault(false);
		connect(backButton, &QPushButton::clicked, this, [this]() {
			if (!source)
				return;
			proc_handler_t *ph = obs_source_get_proc_handler(source);
			if (ph)
				proc_handler_call(ph, "browser_go_back", nullptr);
		});
		headerLayout->addWidget(backButton);

		auto *refreshButton = new QPushButton(QTStr("Basic.InteractionWindow.Refresh"), this);
		refreshButton->setObjectName(QStringLiteral("androidInteractionRefreshButton"));
		refreshButton->setMinimumSize(64, 36);
		refreshButton->setAutoDefault(false);
		refreshButton->setDefault(false);
		connect(refreshButton, &QPushButton::clicked, this, [this]() {
			if (!source)
				return;
			proc_handler_t *ph = obs_source_get_proc_handler(source);
			if (ph)
				proc_handler_call(ph, "browser_reload", nullptr);
		});
		headerLayout->addWidget(refreshButton);
	}

	auto *closeButton = new QPushButton(QTStr("Close"), this);
	closeButton->setObjectName(QStringLiteral("androidInteractionCloseButton"));
	closeButton->setMinimumSize(72, 36);
	closeButton->setAutoDefault(false);
	closeButton->setDefault(false);
	connect(closeButton, &QPushButton::clicked, this, &OBSBasicInteraction::close);
	headerLayout->addWidget(closeButton);
	ui->verticalLayout->insertLayout(0, headerLayout);
	ui->verticalLayout->setStretchFactor(ui->preview, 1);

	longPressTimer = new QTimer(this);
	longPressTimer->setSingleShot(true);
	longPressTimer->setInterval(500);
	connect(longPressTimer, &QTimer::timeout, this, &OBSBasicInteraction::TriggerLongPressRightClick);
	ui->preview->setAttribute(Qt::WA_AcceptTouchEvents, true);
#endif

	ui->preview->setMouseTracking(true);
	ui->preview->setFocusPolicy(Qt::StrongFocus);
	ui->preview->installEventFilter(eventFilter.get());

	if (cx > 400 && cy > 400) {
		resize(cx, cy);
	}

	const char *name = obs_source_get_name(source);
	setWindowTitle(QTStr("Basic.InteractionWindow").arg(QT_UTF8(name)));
#ifdef __ANDROID__
	titleLabel->setText(windowTitle());
	androidFitTopLevelToScreen(this);
#endif

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
}

OBSBasicInteraction::~OBSBasicInteraction()
{
#ifdef __ANDROID__
	if (longPressTimer)
		longPressTimer->stop();
#endif
	// since QT fakes a mouse movement while destructing a widget
	// remove our event filter
	ui->preview->removeEventFilter(eventFilter.get());
}

OBSEventFilter *OBSBasicInteraction::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *, QEvent *event) {
		switch (event->type()) {
#ifdef __ANDROID__
		case QEvent::TouchBegin:
		case QEvent::TouchUpdate:
		case QEvent::TouchEnd:
		case QEvent::TouchCancel:
			return this->HandleTouchEvent(static_cast<QTouchEvent *>(event));
#endif
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			return this->HandleMouseClickEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
		case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));

		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			return this->HandleFocusEvent(static_cast<QFocusEvent *>(event));
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return this->HandleKeyEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

void OBSBasicInteraction::SourceRemoved(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<OBSBasicInteraction *>(data), &OBSBasicInteraction::close);
}

void OBSBasicInteraction::SourceRenamed(void *data, calldata_t *params)
{
	const char *name = calldata_string(params, "new_name");
	QString title = QTStr("Basic.InteractionWindow").arg(QT_UTF8(name));

	QMetaObject::invokeMethod(static_cast<OBSBasicProperties *>(data), &OBSBasicInteraction::setWindowTitle, title);
}

void OBSBasicInteraction::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	OBSBasicInteraction *window = static_cast<OBSBasicInteraction *>(data);

	if (!window->source) {
		return;
	}

	uint32_t sourceCX = max(obs_source_get_width(window->source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(window->source), 1u);

	int x, y;
	int newCX, newCY;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);

	newCX = int(scale * float(sourceCX));
	newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(window->source);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

void OBSBasicInteraction::closeEvent(QCloseEvent *event)
{
	QDialog::closeEvent(event);
	if (!event->isAccepted()) {
		return;
	}
#ifdef __ANDROID__
	if (longPressTimer)
		longPressTimer->stop();
#endif

	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cx", width());
	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cy", height());

	obs_display_remove_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
}

bool OBSBasicInteraction::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_MOVE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnMove();
		}
		break;
	case WM_DISPLAYCHANGE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnDisplayChange();
		}
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (event->modifiers().testFlag(Qt::ShiftModifier)) {
		obsModifiers |= INTERACT_SHIFT_KEY;
	}
	if (event->modifiers().testFlag(Qt::AltModifier)) {
		obsModifiers |= INTERACT_ALT_KEY;
	}
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (event->modifiers().testFlag(Qt::ControlModifier)) {
		obsModifiers |= INTERACT_COMMAND_KEY;
	}
	if (event->modifiers().testFlag(Qt::MetaModifier)) {
		obsModifiers |= INTERACT_CONTROL_KEY;
	}
#else
	// Handle windows key? Can a browser even trap that key?
	if (event->modifiers().testFlag(Qt::ControlModifier)) {
		obsModifiers |= INTERACT_CONTROL_KEY;
	}
#endif

	if (!mouseEvent) {
		if (event->modifiers().testFlag(Qt::KeypadModifier)) {
			obsModifiers |= INTERACT_IS_KEY_PAD;
		}
	}

	return obsModifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	int modifiers = TranslateQtKeyboardEventModifiers(event, true);

	if (event->buttons().testFlag(Qt::LeftButton)) {
		modifiers |= INTERACT_MOUSE_LEFT;
	}
	if (event->buttons().testFlag(Qt::MiddleButton)) {
		modifiers |= INTERACT_MOUSE_MIDDLE;
	}
	if (event->buttons().testFlag(Qt::RightButton)) {
		modifiers |= INTERACT_MOUSE_RIGHT;
	}

	return modifiers;
}

bool OBSBasicInteraction::GetSourceRelativeXY(int mouseX, int mouseY, int &relX, int &relY)
{
	float pixelRatio = devicePixelRatioF();
	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = GetPixelSize(ui->preview);

	uint32_t sourceCX = max(obs_source_get_width(source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(source), 1u);

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	if (x > 0) {
		relX = int(float(mouseXscaled - x) / scale);
		relY = int(float(mouseYscaled / scale));
	} else {
		relX = int(float(mouseXscaled / scale));
		relY = int(float(mouseYscaled - y) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX)) {
		return false;
	}
	if (relY < 0 || relY > int(sourceCY)) {
		return false;
	}

	return true;
}

bool OBSBasicInteraction::HandleMouseClickEvent(QMouseEvent *event)
{
	bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	int clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick) {
		clickCount = 2;
	}

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	case Qt::RightButton:
		button = MOUSE_RIGHT;
		break;
	default:
		blog(LOG_WARNING, "unknown button type %d", event->button());
		return false;
	}

	// Why doesn't this work?
	//if (event->flags().testFlag(Qt::MouseEventCreatedDoubleClick))
	//	clickCount = 2;

	QPoint pos = event->pos();
	bool insideSource = GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);

#ifdef __ANDROID__
	if (button == MOUSE_LEFT) {
		if (mouseUp) {
			if (longPressTimer)
				longPressTimer->stop();
			if (longPressTriggered || suppressNextLeftRelease) {
				longPressTriggered = false;
				suppressNextLeftRelease = false;
				return true;
			}
		} else {
			longPressTriggered = false;
			suppressNextLeftRelease = false;
			if (longPressTimer)
				longPressTimer->stop();
			if (insideSource && !twoFingerActive && event->type() == QEvent::MouseButtonPress &&
			    event->source() != Qt::MouseEventNotSynthesized && longPressTimer) {
				longPressWidgetPos = pos;
				longPressRelX = mouseEvent.x;
				longPressRelY = mouseEvent.y;
				longPressTimer->start();
			}
		}
	} else if (longPressTimer) {
		longPressTimer->stop();
	}
#endif

	if (mouseUp || insideSource) {
		obs_source_send_mouse_click(source, &mouseEvent, button, mouseUp, clickCount);
	}

	return true;
}

bool OBSBasicInteraction::HandleMouseMoveEvent(QMouseEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	bool mouseLeave = event->type() == QEvent::Leave;

	if (!mouseLeave) {
		mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);
		QPoint pos = event->pos();
#ifdef __ANDROID__
		if (longPressTimer && longPressTimer->isActive()) {
			const int dx = pos.x() - longPressWidgetPos.x();
			const int dy = pos.y() - longPressWidgetPos.y();
			if (dx * dx + dy * dy > 12 * 12)
				longPressTimer->stop();
		}
		if (twoFingerActive || longPressTriggered)
			return true;
#endif
		mouseLeave = !GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);
	}
#ifdef __ANDROID__
	else if (longPressTimer) {
		longPressTimer->stop();
	}
#endif

	obs_source_send_mouse_move(source, &mouseEvent, mouseLeave);

	return true;
}

#ifdef __ANDROID__
void OBSBasicInteraction::TriggerLongPressRightClick()
{
	if (!source || twoFingerActive)
		return;
	struct obs_mouse_event cancelEvent = {};
	obs_source_send_mouse_move(source, &cancelEvent, true);

	struct obs_mouse_event rightEvent = {};
	rightEvent.x = longPressRelX;
	rightEvent.y = longPressRelY;
	longPressTriggered = true;
	obs_source_send_mouse_click(source, &rightEvent, MOUSE_RIGHT, false, 1);
	obs_source_send_mouse_click(source, &rightEvent, MOUSE_RIGHT, true, 1);
}

bool OBSBasicInteraction::HandleTouchEvent(QTouchEvent *event)
{
	if (!source || !event)
		return false;

	const auto &points = event->points();
	if (event->type() == QEvent::TouchCancel || points.isEmpty()) {
		if (longPressTimer)
			longPressTimer->stop();
		if (twoFingerActive) {
			twoFingerActive = false;
			suppressNextLeftRelease = true;
			event->accept();
			return true;
		}
		return false;
	}

	if (points.size() >= 2) {
		if (longPressTimer)
			longPressTimer->stop();
		const QPointF mid = (points[0].position() + points[1].position()) * 0.5;
		if (!twoFingerActive) {
			struct obs_mouse_event cancelEvent = {};
			obs_source_send_mouse_move(source, &cancelEvent, true);
			twoFingerActive = true;
			suppressNextLeftRelease = true;
			longPressTriggered = false;
			lastTwoFingerMid = mid;
		} else {
			const QPointF delta = mid - lastTwoFingerMid;
			lastTwoFingerMid = mid;
			struct obs_mouse_event wheelEvent = {};
			if (GetSourceRelativeXY(int(std::lround(mid.x())), int(std::lround(mid.y())), wheelEvent.x,
						wheelEvent.y)) {
				const int dx = int(std::lround(delta.x() * 3.0));
				const int dy = int(std::lround(delta.y() * 3.0));
				if (dx != 0 || dy != 0)
					obs_source_send_mouse_wheel(source, &wheelEvent, dx, dy);
			}
		}
		event->accept();
		return true;
	}

	if (twoFingerActive) {
		if (event->type() == QEvent::TouchEnd)
			twoFingerActive = false;
		event->accept();
		return true;
	}

	return false;
}
#endif

bool OBSBasicInteraction::HandleMouseWheelEvent(QWheelEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x()) {
			xDelta = event->pixelDelta().x();
		} else {
			yDelta = event->pixelDelta().y();
		}
	} else {
		if (angleDelta.x()) {
			xDelta = angleDelta.x();
		} else {
			yDelta = angleDelta.y();
		}
	}

	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();

	if (GetSourceRelativeXY(x, y, mouseEvent.x, mouseEvent.y)) {
		obs_source_send_mouse_wheel(source, &mouseEvent, xDelta, yDelta);
	}

	return true;
}

bool OBSBasicInteraction::HandleFocusEvent(QFocusEvent *event)
{
	bool focus = event->type() == QEvent::FocusIn;

	obs_source_send_focus(source, focus);

	return true;
}

bool OBSBasicInteraction::HandleKeyEvent(QKeyEvent *event)
{
	struct obs_key_event keyEvent;

	QByteArray text = event->text().toUtf8();
	keyEvent.modifiers = TranslateQtKeyboardEventModifiers(event, false);
	keyEvent.text = text.data();
	keyEvent.native_modifiers = event->nativeModifiers();
	keyEvent.native_scancode = event->nativeScanCode();
	keyEvent.native_vkey = event->nativeVirtualKey();

	bool keyUp = event->type() == QEvent::KeyRelease;

	obs_source_send_key_click(source, &keyEvent, keyUp);

	return true;
}

void OBSBasicInteraction::Init()
{
	show();
}
