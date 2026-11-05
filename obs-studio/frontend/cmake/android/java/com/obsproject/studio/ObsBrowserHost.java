package com.obsproject.studio;

import android.app.Presentation;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.graphics.Color;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import org.json.JSONObject;
import java.util.concurrent.atomic.AtomicBoolean;

/** One offscreen browser per OBS source. No MediaProjection or CPU frame copy. */
public final class ObsBrowserHost {
    private static final String TAG = "OBS-browser";
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean frame = new AtomicBoolean();
    private final int[] binding = new int[1]; // update() is synchronized; no per-frame allocation.
    private final Context context;
    private final SurfaceTexture texture;
    private final Surface surface;
    private final int width, height;
    private final String url, css;
    private volatile boolean closed, failed;
    // The following fields are touched only on the main thread.
    private VirtualDisplay display;
    private Presentation presentation;
    private FrameLayout container;
    private WebView web;
    private boolean touching;
    private long downTime;

    private ObsBrowserHost(Context context, int id, int width, int height, String url, String css) {
        this.context = context.getApplicationContext();
        this.width = width; this.height = height; this.url = url; this.css = css;
        SurfaceTexture candidate = new SurfaceTexture(id);
        Surface candidateSurface;
        try {
            candidate.setDefaultBufferSize(width, height);
            candidate.setOnFrameAvailableListener(t -> frame.set(true), ui);
            candidateSurface = new Surface(candidate);
        } catch (RuntimeException e) {
            candidate.release();
            throw e;
        }
        texture = candidate;
        surface = candidateSurface;
        ui.post(this::startOnUi);
    }
    public static ObsBrowserHost create(Context context, int id, int width, int height, String url, String css) {
        if (context == null || id == 0 || width <= 0 || height <= 0) return null;
        try { return new ObsBrowserHost(context, id, width, height, url, css); }
        catch (RuntimeException e) { Log.e(TAG, "SurfaceTexture creation failed", e); return null; }
    }
    private void startOnUi() {
        if (closed) return;
        try {
            DisplayManager manager = (DisplayManager)context.getSystemService(Context.DISPLAY_SERVICE);
            display = manager.createVirtualDisplay("OBS browser", width, height,
                    context.getResources().getDisplayMetrics().densityDpi, surface,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION);
            if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
            presentation = new Presentation(context, display.getDisplay());
            container = new FrameLayout(presentation.getContext());
            presentation.setContentView(container);
            presentation.show();
            createWebSafe();
        } catch (RuntimeException e) { Log.e(TAG, "Browser display creation failed", e); failed = true; cleanupUi(); }
    }
    private void createWebSafe() {
        if (closed) return;
        try { createWeb(); }
        catch (RuntimeException e) { Log.e(TAG, "WebView creation failed", e); failed = true; cleanupUi(); }
    }
    private void createWeb() {
        if (closed || container == null) return;
        web = new WebView(presentation.getContext());
        web.setBackgroundColor(Color.TRANSPARENT);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.getSettings().setAllowFileAccess(false);
        web.getSettings().setAllowContentAccess(false);
        web.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView view, String pageUrl) {
                if (closed || view != web) return;
                if (css != null && !css.isEmpty()) {
                    String quoted = JSONObject.quote(css);
                    view.evaluateJavascript("(function(){var s=document.getElementById('__obs_custom_css');" +
                            "if(!s){s=document.createElement('style');s.id='__obs_custom_css';document.head.appendChild(s);}" +
                            "s.textContent=" + quoted + ";})()", null);
                }
            }
            @Override public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
                if (view != web) return true;
                Log.w(TAG, "WebView renderer exited; recreating");
                if (container != null) container.removeView(view);
                view.destroy();
                if (web == view) { web = null; touching = false; }
                if (!closed) ui.postDelayed(ObsBrowserHost.this::createWebSafe, 1000);
                return true;
            }
        });
        container.addView(web, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        web.loadUrl(url == null || url.isEmpty() ? "about:blank" : url);
    }
    /** Called only with OBS's EGL context current. */
    public synchronized int update(float[] matrix) {
        if (closed || failed) return -1;
        if (!frame.getAndSet(false)) return 0;
        // updateTexImage implicitly binds GL_TEXTURE_EXTERNAL_OES on the current
        // active unit. Restore that binding so OBS's GL state stays consistent.
        GLES20.glGetIntegerv(0x8D67 /* GL_TEXTURE_BINDING_EXTERNAL_OES */, binding, 0);
        try { texture.updateTexImage(); texture.getTransformMatrix(matrix); return 1; }
        catch (RuntimeException e) { Log.e(TAG, "updateTexImage failed", e); return -1; }
        finally { GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, binding[0]); }
    }
    public void reload() { ui.post(() -> { if (!closed && web != null) web.reload(); }); }
    public void setVisible(boolean visible) { ui.post(() -> { if (!closed && web != null) { if (visible) web.onResume(); else web.onPause(); } }); }
    public void touch(int x, int y, int action) {
        ui.post(() -> {
            if (closed || web == null) return;
            long now = SystemClock.uptimeMillis();
            if (action == MotionEvent.ACTION_DOWN) {
                if (touching) dispatchTouch(MotionEvent.ACTION_CANCEL, x, y, now);
                downTime = now; touching = true;
                web.requestFocus();
            } else if (!touching) return;
            dispatchTouch(action, x, y, now);
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) touching = false;
        });
    }
    private void dispatchTouch(int action, int x, int y, long now) {
        MotionEvent event = MotionEvent.obtain(downTime, now, action, x, y, 0);
        try { web.dispatchTouchEvent(event); } finally { event.recycle(); }
    }
    /** No UI-thread wait: the producer is disconnected asynchronously. */
    public synchronized void close() {
        if (closed) return;
        closed = true;
        texture.setOnFrameAvailableListener(null);
        surface.release();
        texture.release();
        ui.post(this::cleanupUi);
    }
    private void cleanupUi() {
        if (web != null) { touching = false; if (container != null) container.removeView(web); web.destroy(); web = null; }
        if (presentation != null) { try { presentation.dismiss(); } catch (RuntimeException e) { Log.w(TAG, "Presentation dismiss failed", e); } presentation = null; }
        if (display != null) { display.release(); display = null; }
        container = null;
    }
}
