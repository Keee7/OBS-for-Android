/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

class QApplication;
class QEvent;
class QObject;

void InstallAndroidAppFilters(QApplication *app);
bool AndroidBeforeAppNotify(QApplication *app, QObject *receiver, QEvent *e, bool &result);
void AndroidAfterAppNotify(QEvent *e);
