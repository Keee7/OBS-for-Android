#include <QApplication>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "../../obs-studio/shared/properties-view/android-ime-diagnostics.hpp"

template<typename Editor> static void checkEditor(Editor &editor, bool enabled)
{
	QPointer<Editor> original(&editor);
	const int childrenBefore = editor.children().size();
	OBSImeDiagnostics::install(&editor);
	assert(editor.children().size() == childrenBefore + (enabled ? 1 : 0));
	editor.setFocus();
	QApplication::processEvents();
	assert(QApplication::focusWidget() == &editor);
	QInputMethodEvent deletion;
	deletion.setCommitString(QString(), -1, 1);
	QApplication::sendEvent(&editor, &deletion);
	QApplication::processEvents();
	assert(original == &editor);
	assert(QApplication::focusWidget() == &editor);
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	const bool enabled = argc > 1 && QByteArray(argv[1]) == "enabled";
	QWidget window;
	QLineEdit line(&window);
	QPlainTextEdit plain(&window);
	line.setText("abcd");
	plain.setPlainText("abcd");
	plain.move(0, 40);
	plain.moveCursor(QTextCursor::End);
	window.resize(400, 300);
	window.show();
	window.activateWindow();
	app.processEvents();
	checkEditor(line, enabled);
	assert(line.text() == "abc" && line.cursorPosition() == 3);
	checkEditor(plain, enabled);
	assert(plain.toPlainText() == "abc" && plain.textCursor().position() == 3);
	return 0;
}
