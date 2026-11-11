// Copyright (C) 2023 The Qt Company Ltd.
// Copyright (C) 2012 BogDan Vatra <bogdan@kde.org>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

package org.qtproject.qt.android;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
/**
    A layout which corresponds to one Activity, i.e. is the root layout where the top level window
    and handles orientation changes.
*/
class QtRootLayout extends QtLayout implements QtNative.AppStateDetailsListener
{
    QtRootLayout(Context context)
    {
        super(context);
    }

    @Override
    protected void onAttachedToWindow()
    {
        super.onAttachedToWindow();
        QtNative.registerAppStateListener(this);
        // The integration may already be ready when an Activity reattaches.
        if (QtNative.getStateDetails().nativePluginIntegrationReady)
            reportReadySize();
    }

    @Override
    protected void onDetachedFromWindow()
    {
        QtNative.unregisterAppStateListener(this);
        super.onDetachedFromWindow();
    }

    @Override
    public void onNativePluginIntegrationReadyChanged(boolean ready)
    {
        if (ready)
            reportReadySize();
    }

    private void reportReadySize()
    {
        // The callback may run on the Qt thread. Read View state on Android's
        // UI thread, and skip stale work after detach or integration shutdown.
        post(() -> {
            if (isAttachedToWindow() && QtNative.getStateDetails().nativePluginIntegrationReady
                    && getWidth() > 0 && getHeight() > 0)
                QtDisplayManager.handleLayoutSizeChanged(getWidth(), getHeight());
        });
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh)
    {
        Activity activity = (Activity) getContext();
        if (activity == null || (w == oldw && h == oldh))
            return;
        QtDisplayManager.handleLayoutSizeChanged(w, h);
    }

    @Override
    public void onConfigurationChanged(Configuration configuration)
    {
        Activity activity = (Activity) getContext();
        if (activity == null)
            return;

        // Post the orientation handling just in case the onSizeChanged() is
        // called a bit late (QTBUG-94459).
        QtNative.runAction(() -> {
            QtDisplayManager.handleOrientationChange(activity);
        });
    }
}
