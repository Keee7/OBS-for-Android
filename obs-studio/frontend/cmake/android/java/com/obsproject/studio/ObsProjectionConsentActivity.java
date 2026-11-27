/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transparent proxy Activity that hosts MediaProjectionManager.createScreenCaptureIntent()
 * and delivers onActivityResult back to ObsProjectionHost.
 *
 * Kept separate from ObsProjectionHost so projection state, VirtualDisplay lifecycle,
 * and controller coordination live in a static utility host like every other Obs*Host class.
 */
package com.obsproject.studio;

import android.app.Activity;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;
import android.util.Log;

public final class ObsProjectionConsentActivity extends Activity {
    private static final int REQUEST_CODE = 0x0B53;

    /** 本实例出生时的那一代。 */
    private int myGen = 0;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        myGen = getIntent().getIntExtra(ObsProjectionHost.EXTRA_CONSENT_GENERATION, 0);
        if (!ObsProjectionHost.beginConsentActivity(myGen)) {
            finish();
            return;
        }
        try {
            final MediaProjectionManager mpm =
                ObsProjectionHost.acquireProjectionManager(getApplicationContext(), myGen);
            if (mpm == null) {
                finish();
                return;
            }
            if (saved == null) {
                startActivityForResult(mpm.createScreenCaptureIntent(), REQUEST_CODE);
                Log.i(ObsProjectionHost.TAG, "已通过 createScreenCaptureIntent 发起系统录屏授权请求");
            }
        } catch (Throwable t) {
            ObsProjectionHost.onConsentLaunchFailed(myGen, t);
            finish();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req == REQUEST_CODE) {
            ObsProjectionHost.onConsentActivityResult(getApplicationContext(), myGen, result, data);
        }
        finish(); // 透明代理，交完手就走（后续那一步用的是静态字段）
    }
}
