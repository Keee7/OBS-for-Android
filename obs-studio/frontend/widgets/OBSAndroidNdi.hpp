#pragma once

#ifdef __ANDROID__
#include <QString>

class QMenu;
class QWidget;

bool obsAndroidNdiRegisterBus(QString *detail = nullptr);
void obsAndroidNdiInitOutputMenu(QWidget *parent, QMenu *toolsMenu);
void obsAndroidNdiShutdownOutput();
#endif
