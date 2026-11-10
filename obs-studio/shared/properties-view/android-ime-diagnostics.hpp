#pragma once

// Opt-in diagnostics only. Never log editor contents, property names or paths.
#if defined(Q_OS_ANDROID) || defined(OBS_IME_DIAGNOSTICS_TEST)
#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace OBSImeDiagnostics {
inline bool enabled()
{
	static const bool value = qgetenv("OBS_ANDROID_IME_DIAGNOSTICS") == "1";
	return value;
}

inline void refresh(const char *reason, const QWidget *view)
{
	if (enabled())
		qInfo().noquote() << "OBS-IME refresh" << reason << "view" << static_cast<const void *>(view)
				  << "focusWidget" << static_cast<const void *>(QApplication::focusWidget())
				  << "focusObject" << static_cast<const void *>(QGuiApplication::focusObject());
}

class Filter : public QObject {
public:
	explicit Filter(QWidget *editor) : QObject(editor), editor(editor) {}

protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		switch (event->type()) {
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			qInfo().noquote() << "OBS-IME focus" << (event->type() == QEvent::FocusIn ? "in" : "out")
					  << "editor" << static_cast<void *>(editor.data()) << "reason"
					  << int(static_cast<QFocusEvent *>(event)->reason()) << "focus"
					  << static_cast<void *>(QApplication::focusWidget()) << "focusObject"
					  << static_cast<void *>(QGuiApplication::focusObject());
			break;
		case QEvent::KeyPress:
		case QEvent::KeyRelease: {
			const auto *key = static_cast<QKeyEvent *>(event);
			if (key->key() != Qt::Key_Backspace && key->key() != Qt::Key_Delete)
				return false;
			qInfo().noquote() << "OBS-IME deleteKey" << (event->type() == QEvent::KeyPress ? "down" : "up")
					  << "editor" << static_cast<void *>(editor.data()) << "repeat" << key->isAutoRepeat()
					  << "focusObject" << static_cast<void *>(QGuiApplication::focusObject());
			break;
		}
		case QEvent::InputMethod: {
			const auto *ime = static_cast<QInputMethodEvent *>(event);
			qInfo().noquote() << "OBS-IME event editor" << static_cast<void *>(editor.data())
					  << "preeditLength" << ime->preeditString().size() << "commitLength"
					  << ime->commitString().size() << "replacementStart" << ime->replacementStart()
					  << "replacementLength" << ime->replacementLength();
			break;
		}
		default:
			return false;
		}
		// Sample after Qt handles the event. Coalesce bursts and never query from a query event.
		if (!pending) {
			pending = true;
			QTimer::singleShot(0, this, [this] {
				pending = false;
				if (!editor || QApplication::focusWidget() != editor)
					return;
				const QRect local = editor->inputMethodQuery(Qt::ImCursorRectangle).toRect();
				QWidget *window = editor->window();
				const QRect global(editor->mapToGlobal(local.topLeft()), local.size());
				const QRect inWindow(editor->mapTo(window, local.topLeft()), local.size());
				qInfo().noquote() << "OBS-IME geometry editor" << static_cast<void *>(editor.data())
						  << "window" << static_cast<void *>(window) << "local" << local
						  << "windowRect" << inWindow << "global" << global
						  << "windowGeometry" << window->geometry() << "dpr" << editor->devicePixelRatioF()
						  << "qtCursorRect" << QGuiApplication::inputMethod()->cursorRectangle();
			});
		}
		return false;
	}
private:
	QPointer<QWidget> editor;
	bool pending = false;
};

inline void install(QWidget *editor)
{
	if (enabled())
		editor->installEventFilter(new Filter(editor));
}
} // namespace OBSImeDiagnostics
#else
namespace OBSImeDiagnostics {
inline void refresh(const char *, const QWidget *) {}
inline void install(QWidget *) {}
}
#endif
