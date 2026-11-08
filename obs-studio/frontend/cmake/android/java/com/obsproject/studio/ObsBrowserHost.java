package com.obsproject.studio;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.graphics.drawable.ColorDrawable;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
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
import android.view.Window;
import android.view.WindowManager;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import org.json.JSONObject;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

/** One offscreen browser per OBS source. No MediaProjection or CPU frame copy. */
public final class ObsBrowserHost {
    private static final String TAG = "OBS-browser";
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean frame = new AtomicBoolean();
    private final int[] binding = new int[1]; // update() is synchronized; no per-frame allocation.
    private final Context context;
    /* Two ways to receive composited frames, chosen by the caller:
     *  - non-zero texture id: SurfaceTexture. Zero copy, but SurfaceFlinger
     *    hands this consumer to the hardware composer, and this device's
     *    composer writes an opaque target, so every alpha value is lost.
     *    Still the cheapest path, used whenever transparency is off.
     *  - texture id 0: ImageReader. Its buffers cannot be scanned out, so the
     *    compositor falls back to GPU composition, which keeps alpha. The
     *    images are sampled on the GPU through an EGLImage - still zero copy,
     *    no CPU readback. */
    private final boolean readerMode;
    private SurfaceTexture texture;
    private Surface surface;
    private ImageReader reader;
    private final Image[] held = new Image[2];
    private int heldNext;
    private final int width, height;
    private final String url, css;
    private final boolean transparent;
    private volatile boolean closed, failed;
    // The following fields are touched only on the main thread.
    private VirtualDisplay display;
    private Presentation presentation;
    private FrameLayout container;
    private WebView web;
    private boolean touching;
    private long downTime;

    private ObsBrowserHost(Context context, int id, int width, int height, String url, String css, boolean transparent) {
        this.context = context.getApplicationContext();
        this.width = width; this.height = height; this.url = url; this.css = css;
        this.transparent = transparent;
        this.readerMode = id == 0;
        if (!readerMode) {
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
        }
        ui.post(this::startOnUi);
    }
    public static ObsBrowserHost create(Context context, int id, int width, int height, String url, String css,
                                        boolean transparent) {
        if (context == null || width <= 0 || height <= 0) return null;
        try { return new ObsBrowserHost(context, id, width, height, url, css, transparent); }
        catch (RuntimeException e) { Log.e(TAG, "Surface creation failed", e); return null; }
    }
    private void startOnUi() {
        if (closed) return;
        try {
            DisplayManager manager = (DisplayManager)context.getSystemService(Context.DISPLAY_SERVICE);
            Surface target = surface;
            if (readerMode) {
                /* CPU usage is what keeps the hardware composer from scanning
                 * this buffer out (measured: with it the compositor falls back
                 * to the GPU and alpha survives), and GPU sampling is what lets
                 * OBS bind it through an EGLImage. Neither costs a copy: the
                 * CPU never touches these pixels. */
                long usage = HardwareBuffer.USAGE_CPU_READ_OFTEN | HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE;
                reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 4, usage);
                reader.setOnImageAvailableListener(r -> frame.set(true), ui);
                target = reader.getSurface();
            }
            display = manager.createVirtualDisplay("OBS browser", width, height,
                    context.getResources().getDisplayMetrics().densityDpi, target,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY |
                            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION);
            if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
            presentation = new Presentation(context, display.getDisplay());
            container = new FrameLayout(presentation.getContext());
            // 非透明模式保持主题背景：强行铺黑会把"自己没画背景"的页面直接压成黑屏。
            if (transparent)
                container.setBackgroundColor(Color.TRANSPARENT);
            // Content must be installed first: PhoneWindow.generateLayout() runs
            // inside setContentView() and re-applies the theme's window
            // background and FLAG_DIM_BEHIND, which would cover the whole
            // virtual display with an opaque/dim layer.
            presentation.setContentView(container);
            configureWindow(presentation.getWindow(), transparent);
            presentation.show();
            Log.i(TAG, "browser " + width + "x" + height + " transparent=" + transparent
                    + " output=" + (readerMode ? "imagereader" : "surfacetexture"));
            createWebSafe();
        } catch (RuntimeException e) { Log.e(TAG, "Browser display creation failed", e); failed = true; cleanupUi(); }
    }
    /** Dialog themes restore their dim layer while setContentView() builds the
     *  decor, so window attributes must be applied afterwards and before
     *  show(). Nothing sits behind this window on an offscreen display, and a
     *  dim layer would fill uncovered pixels with translucent black. */
    private static void configureWindow(Window window, boolean transparent) {
        if (window == null) return;
        // Force the decor first: installing it runs generateLayout(), which
        // re-applies the theme's dim flag and window background.
        window.getDecorView();
        window.setDimAmount(0f);
        window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        if (!transparent)
            return;
        window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        window.getDecorView().setBackgroundColor(Color.TRANSPARENT);
        // RGBA window surface; the default floating-dialog format can be
        // opaque even when the views themselves are clear.
        window.setFormat(PixelFormat.TRANSLUCENT);
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
                // Author styles are appended after this one, so a page that
                // paints its own background keeps it.
                if (transparent) {
                    view.evaluateJavascript("(function(){if(document.getElementById('__obs_transparent'))return;" +
                            "var s=document.createElement('style');s.id='__obs_transparent';" +
                            "s.textContent='html,body{background-color:transparent}';" +
                            "document.head.insertBefore(s,document.head.firstChild);})()", null);
                }
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
        if (readerMode) {
            if (!frame.getAndSet(false)) return 0;
            /* The composited image is already top-down; OBS's interop shader
             * does the single flip, so the producer transform is identity. */
            for (int i = 0; i < 16; i++) matrix[i] = (i % 5 == 0) ? 1f : 0f;
            return 1;
        }
        if (!frame.getAndSet(false)) return 0;
        // updateTexImage implicitly binds GL_TEXTURE_EXTERNAL_OES on the current
        // active unit. Restore that binding so OBS's GL state stays consistent.
        GLES20.glGetIntegerv(0x8D67 /* GL_TEXTURE_BINDING_EXTERNAL_OES */, binding, 0);
        try { texture.updateTexImage(); texture.getTransformMatrix(matrix); return 1; }
        catch (RuntimeException e) { Log.e(TAG, "updateTexImage failed", e); return -1; }
        finally { GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, binding[0]); }
    }
    /** Reader mode only. One frame of slack is kept on purpose: the buffer that
     *  was handed out last frame may still be sampled by the GPU command stream
     *  we submitted, so it is released two acquisitions later. Zero copy — the
     *  pixels go straight from the compositor's buffer into OBS's texture. */
    public synchronized HardwareBuffer takePendingBuffer() {
        if (closed || reader == null) return null;
        Image image;
        try { image = reader.acquireLatestImage(); }
        catch (RuntimeException e) { return null; }
        if (image == null) return null;
        Image old = held[heldNext];
        held[heldNext] = image;
        heldNext ^= 1;
        if (old != null) { try { old.close(); } catch (RuntimeException e) { } }
        return image.getHardwareBuffer();
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
        if (texture != null) {
            texture.setOnFrameAvailableListener(null);
            surface.release();
            texture.release();
            surface = null;
            texture = null;
        }
        ui.post(this::cleanupUi);
    }
    private void cleanupUi() {
        for (int i = 0; i < held.length; i++) {
            if (held[i] != null) { try { held[i].close(); } catch (RuntimeException e) { } held[i] = null; }
        }
        if (reader != null) { reader.close(); reader = null; }
        if (web != null) { touching = false; if (container != null) container.removeView(web); web.destroy(); web = null; }
        if (presentation != null) { try { presentation.dismiss(); } catch (RuntimeException e) { Log.w(TAG, "Presentation dismiss failed", e); } presentation = null; }
        if (display != null) { display.release(); display = null; }
        container = null;
    }
}
