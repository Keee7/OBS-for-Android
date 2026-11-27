/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#ifdef __ANDROID__

#include <QJniObject>

inline QJniObject obsAndroidActivity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

inline QJniObject obsAndroidContext()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "getContext",
						  "()Landroid/content/Context;");
}

inline QJniObject obsAndroidAppContext()
{
	if (QJniObject ctx = obsAndroidContext(); ctx.isValid())
		return ctx;
	return obsAndroidActivity();
}

#endif
