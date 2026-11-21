#pragma once

#include <widgets/OBSQTDisplay.hpp>

#include <QObject>
#include <QPlatformSurfaceEvent>

class SurfaceEventFilter : public QObject {
	Q_OBJECT

	OBSQTDisplay *display;

public:
	SurfaceEventFilter(OBSQTDisplay *src) : QObject(src), display(src) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		bool result = QObject::eventFilter(obj, event);
		QPlatformSurfaceEvent *surfaceEvent;

		switch (event->type()) {
		case QEvent::PlatformSurface:
			surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);

			switch (surfaceEvent->surfaceEventType()) {
			case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
#ifndef __ANDROID__
				display->DestroyDisplay();
#endif
				// Android display lifetime follows its TextureView's callbacks.
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}

		return result;
	}
};
