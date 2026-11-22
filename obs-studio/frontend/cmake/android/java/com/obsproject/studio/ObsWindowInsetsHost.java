package com.obsproject.studio;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.DisplayCutout;
import android.view.RoundedCorner;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Computes physical-pixel safe insets covering system bars, display cutouts,
 * edge gesture zones, and large rounded corners on modern full-screen phones.
 */
public final class ObsWindowInsetsHost {
    private static final Handler UI_HANDLER = new Handler(Looper.getMainLooper());
    private static final float PHONE_SIDE_EDGE_DP = 16.0f;
    private static final float EDGE_RIM_DP = 6.0f;

    private ObsWindowInsetsHost() {}

    public static int[] getSafeInsetsPx(Activity activity, View targetView) {
        if (activity == null)
            return new int[]{0, 0, 0, 0};

        if (Looper.myLooper() == Looper.getMainLooper()) {
            return computeOnUiThread(activity, targetView);
        }

        final AtomicReference<int[]> ref = new AtomicReference<>();
        final CountDownLatch latch = new CountDownLatch(1);
        UI_HANDLER.post(() -> {
            try {
                ref.set(computeOnUiThread(activity, targetView));
            } finally {
                latch.countDown();
            }
        });
        try {
            latch.await(100, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        int[] snapshot = ref.get();
        return snapshot != null ? snapshot : computePhoneFallbackOnly(activity);
    }

    private static int[] computePhoneFallbackOnly(Activity activity) {
        Configuration config = activity.getResources().getConfiguration();
        boolean phoneFullscreen = !activity.isInMultiWindowMode() && config.smallestScreenWidthDp < 600;
        if (!phoneFullscreen)
            return new int[]{0, 0, 0, 0};
        float density = activity.getResources().getDisplayMetrics().density;
        int minSidePx = Math.round(PHONE_SIDE_EDGE_DP * density);
        int rimPx = Math.round(EDGE_RIM_DP * density);
        return new int[]{minSidePx, rimPx, minSidePx, rimPx};
    }

    private static int[] computeOnUiThread(Activity activity, View targetView) {
        Window window = activity.getWindow();
        if (window == null)
            return computePhoneFallbackOnly(activity);

        View decor = window.getDecorView();
        View view = targetView != null ? targetView : decor;
        if (view == null)
            return computePhoneFallbackOnly(activity);

        View root = view.getRootView();
        if (root == null)
            root = decor != null ? decor : view;

        WindowInsets insets = root.getRootWindowInsets();

        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        int rTL = 0;
        int rTR = 0;
        int rBL = 0;
        int rBR = 0;

        if (insets != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                Insets barsAndCutout = insets.getInsets(
                        WindowInsets.Type.displayCutout() | WindowInsets.Type.systemBars());
                Insets gestures = insets.getInsets(WindowInsets.Type.systemGestures());
                Insets mandatory = insets.getInsets(WindowInsets.Type.mandatorySystemGestures());
                left = Math.max(barsAndCutout.left, gestures.left);
                top = barsAndCutout.top;
                right = Math.max(barsAndCutout.right, gestures.right);
                bottom = Math.max(barsAndCutout.bottom, Math.max(gestures.bottom, mandatory.bottom));
            } else {
                Insets legacy = readLegacyInsets(insets);
                left = legacy.left;
                top = legacy.top;
                right = legacy.right;
                bottom = legacy.bottom;
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                rTL = cornerRadius(insets, RoundedCorner.POSITION_TOP_LEFT);
                rTR = cornerRadius(insets, RoundedCorner.POSITION_TOP_RIGHT);
                rBL = cornerRadius(insets, RoundedCorner.POSITION_BOTTOM_LEFT);
                rBR = cornerRadius(insets, RoundedCorner.POSITION_BOTTOM_RIGHT);
            }
        }

        float density = activity.getResources().getDisplayMetrics().density;
        int rimPx = Math.round(EDGE_RIM_DP * density);
        int maxTopR = Math.max(rTL, rTR);
        int maxBottomR = Math.max(rBL, rBR);
        if (top == 0 && maxTopR > 0)
            top = Math.min(rimPx, maxTopR);
        if (bottom == 0 && maxBottomR > 0)
            bottom = Math.min(rimPx, maxBottomR);

        Configuration config = activity.getResources().getConfiguration();
        boolean phoneFullscreen = !activity.isInMultiWindowMode() && config.smallestScreenWidthDp < 600;
        if (phoneFullscreen) {
            top = Math.max(top, rimPx);
            bottom = Math.max(bottom, rimPx);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && insets != null) {
            int rootWidth = root.getWidth();
            left = Math.max(left, Math.max(
                    cornerHorizontalInset(rTL, top, rootWidth),
                    cornerHorizontalInset(rBL, bottom, rootWidth)));
            right = Math.max(right, Math.max(
                    cornerHorizontalInset(rTR, top, rootWidth),
                    cornerHorizontalInset(rBR, bottom, rootWidth)));
        }

        if (phoneFullscreen) {
            int minSidePx = Math.round(PHONE_SIDE_EDGE_DP * density);
            left = Math.max(left, minSidePx);
            right = Math.max(right, minSidePx);
        }

        int leftOffset = 0;
        int topOffset = 0;
        int rightOffset = 0;
        int bottomOffset = 0;
        if (view != root && root.getWidth() > 0 && root.getHeight() > 0
                && view.getWidth() > 0 && view.getHeight() > 0) {
            int[] rootLoc = new int[2];
            int[] viewLoc = new int[2];
            root.getLocationOnScreen(rootLoc);
            view.getLocationOnScreen(viewLoc);
            leftOffset = Math.max(0, viewLoc[0] - rootLoc[0]);
            topOffset = Math.max(0, viewLoc[1] - rootLoc[1]);
            rightOffset = Math.max(0, (rootLoc[0] + root.getWidth()) - (viewLoc[0] + view.getWidth()));
            bottomOffset = Math.max(0, (rootLoc[1] + root.getHeight()) - (viewLoc[1] + view.getHeight()));
        }

        return new int[]{
                Math.max(0, left - leftOffset),
                Math.max(0, top - topOffset),
                Math.max(0, right - rightOffset),
                Math.max(0, bottom - bottomOffset)
        };
    }

    private static int cornerHorizontalInset(int radiusPx, int verticalInsetPx, int rootWidth) {
        if (radiusPx <= 0 || verticalInsetPx >= radiusPx)
            return 0;
        int inset;
        if (verticalInsetPx <= 0) {
            inset = radiusPx;
        } else {
            double dy = radiusPx - verticalInsetPx;
            inset = (int) Math.ceil(radiusPx - Math.sqrt(radiusPx * (double) radiusPx - dy * dy));
        }
        if (rootWidth > 0)
            inset = Math.min(inset, rootWidth / 12);
        return Math.max(0, inset);
    }

    private static int cornerRadius(WindowInsets insets, int position) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S)
            return 0;
        RoundedCorner corner = insets.getRoundedCorner(position);
        return corner != null ? Math.max(0, corner.getRadius()) : 0;
    }

    @SuppressWarnings("deprecation")
    private static Insets readLegacyInsets(WindowInsets insets) {
        int left = insets.getSystemWindowInsetLeft();
        int top = insets.getSystemWindowInsetTop();
        int right = insets.getSystemWindowInsetRight();
        int bottom = insets.getSystemWindowInsetBottom();

        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout != null) {
            left = Math.max(left, cutout.getSafeInsetLeft());
            top = Math.max(top, cutout.getSafeInsetTop());
            right = Math.max(right, cutout.getSafeInsetRight());
            bottom = Math.max(bottom, cutout.getSafeInsetBottom());
        }

        Insets gestures = insets.getSystemGestureInsets();
        Insets mandatory = insets.getMandatorySystemGestureInsets();
        left = Math.max(left, gestures.left);
        right = Math.max(right, gestures.right);
        bottom = Math.max(bottom, Math.max(gestures.bottom, mandatory.bottom));
        return Insets.of(left, top, right, bottom);
    }
}
