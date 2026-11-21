#pragma once

#ifdef __ANDROID__
#include <QJniObject>
#include <QtGlobal>

#include <functional>

class QObject;
class QWindow;

// Hosts one OBS display inside the native view of its Qt widget.
class OBSAndroidDisplay {
public:
	OBSAndroidDisplay(QObject *context, std::function<void()> changed);
	~OBSAndroidDisplay();

	bool attach(QWindow *window);
	void *acquireWindow() const;
	void surfaceChanged(const QJniObject &surface, int generation);

private:
	qint64 id;
	std::function<void()> changed;
	QJniObject host;
	QJniObject parent;
	QJniObject surface;
};

void obsAndroidDisplayLog(const char *fmt, ...);
#endif
