package com.obsproject.studio;

import android.app.Presentation;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.graphics.drawable.ColorDrawable;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.Log;
import android.view.Surface;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import org.json.JSONObject;
import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/** One offscreen browser per OBS source. No MediaProjection or CPU frame copy. */
public final class ObsBrowserHost {
    private static final String TAG = "OBS-browser";
    private static final String LOCAL_VIRTUAL_HOST = "obs-local.invalid";
    private static final int AUDIO_PACKET_FRAMES = 1024;
    private static final String AUDIO_HOOK_JS =
            "(function(){" +
            "if(window.__obsAudioHooked){if(window.__obsScanMedia)window.__obsScanMedia();return;}" +
            "window.__obsAudioHooked=true;" +
            "var AC=window.AudioContext||window.webkitAudioContext;" +
            "if(!AC)return;" +
            "var origConnect=AudioNode.prototype.connect;" +
            "var origDisconnect=AudioNode.prototype.disconnect;" +
            "function getTap(ctx){" +
            "if(ctx.__obsTap)return ctx.__obsTap;" +
            "var gain=ctx.createGain();" +
            "var proc=ctx.createScriptProcessor(1024,2,2);" +
            "var silentTail=0;" +
            "proc.onaudioprocess=function(ev){" +
            "var inBuf=ev.inputBuffer,outBuf=ev.outputBuffer;" +
            "var nCh=inBuf.numberOfChannels;" +
            "var L=inBuf.getChannelData(0);" +
            "var R=nCh>1?inBuf.getChannelData(1):L;" +
            "var len=L.length,outCh=outBuf.numberOfChannels;" +
            "for(var c=0;c<outCh;c++)outBuf.getChannelData(c).fill(0);" +
            "var maxAbs=0;" +
            "for(var i=0;i<len;i++){" +
            "var a=Math.abs(L[i]),b=Math.abs(R[i]);" +
            "if(a>maxAbs)maxAbs=a;if(b>maxAbs)maxAbs=b;" +
            "}" +
            "if(maxAbs>1e-5){silentTail=10;}" +
            "else if(silentTail>0){silentTail--;}" +
            "else{return;}" +
            "var pcm=new Int16Array(len*2);" +
            "for(var j=0;j<len;j++){" +
            "var sl=L[j];if(sl>1)sl=1;else if(sl<-1)sl=-1;" +
            "var sr=R[j];if(sr>1)sr=1;else if(sr<-1)sr=-1;" +
            "pcm[j*2]=sl<0?(sl*32768)|0:(sl*32767)|0;" +
            "pcm[j*2+1]=sr<0?(sr*32768)|0:(sr*32767)|0;" +
            "}" +
            "var bytes=new Uint8Array(pcm.buffer),bin='';" +
            "for(var k=0;k<bytes.length;k+=1024){" +
            "bin+=String.fromCharCode.apply(null,bytes.subarray(k,Math.min(k+1024,bytes.length)));" +
            "}" +
            "try{if(window.__obsAudio&&window.__obsAudio.pushPcm){" +
            "window.__obsAudio.pushPcm(btoa(bin),Math.round(ctx.sampleRate)||48000,2);" +
            "}}catch(e){}" +
            "};" +
            "origConnect.call(gain,proc);" +
            "origConnect.call(proc,ctx.destination);" +
            "ctx.__obsTap={gain:gain,proc:proc};" +
            "return ctx.__obsTap;" +
            "}" +
            "AudioNode.prototype.connect=function(dest){" +
            "var ctx=this.context;" +
            "if(ctx&&dest===ctx.destination){" +
            "var tap=getTap(ctx);" +
            "if(this!==tap.proc){" +
            "if(ctx.state==='suspended'&&ctx.resume)ctx.resume().catch(function(){});" +
            "origConnect.call(this,tap.gain,arguments.length>1?arguments[1]:0,0);" +
            "return dest;" +
            "}}" +
            "return origConnect.apply(this,arguments);" +
            "};" +
            "AudioNode.prototype.disconnect=function(dest){" +
            "var ctx=this.context;" +
            "if(ctx&&dest===ctx.destination&&ctx.__obsTap&&this!==ctx.__obsTap.proc){" +
            "try{return origDisconnect.call(this,ctx.__obsTap.gain);}catch(e){return;}" +
            "}" +
            "return origDisconnect.apply(this,arguments);" +
            "};" +
            "var mediaCtx=null;" +
            "function getMediaCtx(){" +
            "if(!mediaCtx)mediaCtx=new AC();" +
            "if(mediaCtx.state==='suspended'&&mediaCtx.resume)mediaCtx.resume().catch(function(){});" +
            "return mediaCtx;" +
            "}" +
            "function isCapturable(el){" +
            "if(el.srcObject)return true;" +
            "var s=el.currentSrc||el.getAttribute('src')||el.src||'';" +
            "if(!s)return true;" +
            "if(s.indexOf('blob:')===0||s.indexOf('data:')===0)return true;" +
            "if(el.crossOrigin)return true;" +
            "try{return new URL(s,location.href).origin===location.origin;}catch(e){return false;}" +
            "}" +
            "function connectMedia(el){" +
            "if(!el||el.__obsAttached)return;" +
            "try{" +
            "var ctx=getMediaCtx();" +
            "var srcNode=ctx.createMediaElementSource(el);" +
            "origConnect.call(srcNode,getTap(ctx).gain);" +
            "el.__obsAttached=true;el.__obsSrcNode=srcNode;" +
            "}catch(e){}" +
            "}" +
            "function attachMedia(el){" +
            "if(!el||el.__obsAttached)return;" +
            "var s=el.currentSrc||el.getAttribute('src')||el.src||'';" +
            "if(!s&&!el.srcObject)return;" +
            "if(isCapturable(el))connectMedia(el);" +
            "}" +
            "var origPlay=HTMLMediaElement.prototype.play;" +
            "if(origPlay){HTMLMediaElement.prototype.play=function(){attachMedia(this);return origPlay.apply(this,arguments);};}" +
            "function scan(){var els=document.querySelectorAll('audio,video');for(var i=0;i<els.length;i++)attachMedia(els[i]);}" +
            "window.__obsScanMedia=scan;" +
            "document.addEventListener('play',function(e){if(e.target&&e.target.tagName)attachMedia(e.target);},true);" +
            "document.addEventListener('playing',function(e){if(e.target&&e.target.tagName)attachMedia(e.target);},true);" +
            "scan();" +
            "var obsRoot=document.documentElement;" +
            "if(obsRoot&&window.MutationObserver){new MutationObserver(function(){scan();}).observe(obsRoot,{childList:true,subtree:true});}" +
            "else{document.addEventListener('DOMContentLoaded',function(){scan();" +
            "if(document.documentElement&&window.MutationObserver){new MutationObserver(function(){scan();}).observe(document.documentElement,{childList:true,subtree:true});}},{once:true});}" +
            "})();";
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean frame = new AtomicBoolean();
    private final int[] binding = new int[1]; // update() is synchronized; no per-frame allocation.
    private final Context context;
    private final Object audioLock = new Object();
    private short[] audioRing;
    private int audioHead, audioTail, audioFilled;
    private int audioRate = 48000, audioChannels = 2;
    private final boolean audioReroute;

    private final class AudioBridge {
        @JavascriptInterface
        public void pushPcm(String base64Pcm, int sampleRate, int channels) {
            if (closed || !audioReroute || base64Pcm == null || base64Pcm.isEmpty())
                return;
            if (sampleRate < 8000 || sampleRate > 192000 || (channels != 1 && channels != 2))
                return;
            final int frameBytes = channels * 2;
            final int maxBytes = AUDIO_PACKET_FRAMES * frameBytes;
            if (base64Pcm.length() > 4 * ((maxBytes + 2) / 3))
                return;
            final byte[] bytes;
            try {
                bytes = Base64.decode(base64Pcm, Base64.DEFAULT);
            } catch (IllegalArgumentException e) {
                return;
            }
            if (bytes.length == 0 || bytes.length > maxBytes || bytes.length % frameBytes != 0)
                return;
            final int frames = bytes.length / frameBytes;
            synchronized (audioLock) {
                if (closed)
                    return;
                if (audioRing == null || audioRate != sampleRate || audioChannels != channels) {
                    audioRate = sampleRate;
                    audioChannels = channels;
                    audioRing = new short[(sampleRate / 2) * channels];
                    audioHead = 0;
                    audioTail = 0;
                    audioFilled = 0;
                }
                for (int f = 0; f < frames; f++) {
                    if (audioFilled + channels > audioRing.length) {
                        int drop = Math.min(channels, audioFilled);
                        audioTail = (audioTail + drop) % audioRing.length;
                        audioFilled -= drop;
                    }
                    final int base = f * frameBytes;
                    for (int ch = 0; ch < channels; ch++) {
                        final int bOff = base + ch * 2;
                        final short sample = (short) ((bytes[bOff + 1] << 8) | (bytes[bOff] & 0xFF));
                        audioRing[audioHead] = sample;
                        audioHead = (audioHead + 1) % audioRing.length;
                        audioFilled++;
                    }
                }
            }
        }
    }
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
    private final Uri localContentUri;
    private final String localVirtualHost;
    private volatile Uri localVirtualUri;
    private volatile String localDisplayName;
    private final boolean allowFileAccess;
    private final boolean transparent;
    private volatile boolean closed, failed;
    // The following fields are touched only on the main thread.
    private VirtualDisplay display;
    private Presentation presentation;
    private FrameLayout container;
    private WebView web;
    private boolean touching;
    private long downTime;

    private ObsBrowserHost(Context context, int id, int width, int height, String url, String css,
                           boolean transparent, boolean audioReroute) {
        this.context = context.getApplicationContext();
        String cleanUrl = url == null ? "" : url.trim();
        this.width = width; this.height = height; this.url = cleanUrl; this.css = css;
        this.transparent = transparent;
        this.audioReroute = audioReroute;
        this.readerMode = id == 0;
        if (cleanUrl.regionMatches(true, 0, "content://", 0, 10)) {
            this.localContentUri = Uri.parse(cleanUrl);
            this.localVirtualHost = localHostForUri(cleanUrl);
            this.allowFileAccess = false;
        } else if (cleanUrl.regionMatches(true, 0, "file://", 0, 7)) {
            this.localContentUri = null;
            this.localVirtualHost = null;
            this.allowFileAccess = true;
        } else {
            this.localContentUri = null;
            this.localVirtualHost = null;
            this.allowFileAccess = false;
        }
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
                                        boolean transparent, boolean audioReroute) {
        if (context == null || width <= 0 || height <= 0) return null;
        try { return new ObsBrowserHost(context, id, width, height, url, css, transparent, audioReroute); }
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
    private void injectAudioHook(WebView view) {
        if (audioReroute && !closed && view == web)
            view.evaluateJavascript(AUDIO_HOOK_JS, null);
    }
    private static String localHostForUri(String uri) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(uri.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(16);
            for (int i = 0; i < 8; i++) {
                hex.append(String.format(Locale.ROOT, "%02x", digest[i] & 0xFF));
            }
            return hex + "." + LOCAL_VIRTUAL_HOST;
        } catch (NoSuchAlgorithmException e) {
            return "doc." + LOCAL_VIRTUAL_HOST;
        }
    }
    private static String resolveDisplayName(Context ctx, Uri uri) {
        String name = null;
        try (Cursor cursor = ctx.getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) name = cursor.getString(idx);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Failed to query display name for " + uri, e);
        }
        if (name == null || name.isEmpty()) {
            name = uri.getLastPathSegment();
        }
        if (name != null) {
            int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
            if (slash >= 0) name = name.substring(slash + 1);
        }
        return (name == null || name.isEmpty() || ".".equals(name) || "..".equals(name))
                ? "index.html" : name;
    }
    private static String normalizeLocalHtmlMime(String rawMime, String displayName) {
        String base = "";
        if (rawMime != null) {
            int semi = rawMime.indexOf(';');
            base = (semi >= 0 ? rawMime.substring(0, semi) : rawMime).trim().toLowerCase(Locale.ROOT);
        }
        if ("text/html".equals(base) || "application/xhtml+xml".equals(base) || "image/svg+xml".equals(base)) {
            return base;
        }
        if (displayName != null) {
            String lower = displayName.toLowerCase(Locale.ROOT);
            if (lower.endsWith(".xhtml") || lower.endsWith(".xht")) return "application/xhtml+xml";
            if (lower.endsWith(".svg")) return "image/svg+xml";
            if (lower.endsWith(".html") || lower.endsWith(".htm")) return "text/html";
        }
        return "text/html";
    }
    private static String extractCharset(String rawMime) {
        if (rawMime == null) return "UTF-8";
        for (String part : rawMime.split(";")) {
            String trimmed = part.trim();
            if (trimmed.regionMatches(true, 0, "charset=", 0, 8)) {
                String charset = trimmed.substring(8).trim();
                if (charset.length() >= 2 && charset.startsWith("\"") && charset.endsWith("\"")) {
                    charset = charset.substring(1, charset.length() - 1).trim();
                }
                if (!charset.isEmpty()) return charset;
            }
        }
        return "UTF-8";
    }
    private static WebResourceResponse localNotFoundResponse() {
        byte[] body = ("<!doctype html><meta charset=utf-8>" +
                "<body style='margin:0;padding:16px;font:16px sans-serif;color:#eee;background:#222'>" +
                "Local HTML file could not be opened.</body>").getBytes(StandardCharsets.UTF_8);
        return new WebResourceResponse("text/html", "UTF-8", 404, "Not Found",
                Collections.singletonMap("Cache-Control", "no-store"), new ByteArrayInputStream(body));
    }
    private static WebResourceResponse emptyErrorResponse(int statusCode, String reason) {
        return new WebResourceResponse("text/plain", "UTF-8", statusCode, reason,
                Collections.singletonMap("Cache-Control", "no-store"), new ByteArrayInputStream(new byte[0]));
    }
    private boolean isDisallowedLocalScheme(Uri reqUri) {
        if (reqUri == null) return false;
        String scheme = reqUri.getScheme();
        if ("content".equalsIgnoreCase(scheme)) return true;
        return !allowFileAccess && "file".equalsIgnoreCase(scheme);
    }
    private WebResourceResponse openLocalDocumentResponse() {
        Map<String, String> headers = Collections.singletonMap("Cache-Control", "no-store");
        String rawMime = null;
        try {
            rawMime = context.getContentResolver().getType(localContentUri);
        } catch (RuntimeException e) {
            Log.w(TAG, "Failed to query MIME type for " + localContentUri, e);
        }
        String mime = normalizeLocalHtmlMime(rawMime, localDisplayName);
        String encoding = extractCharset(rawMime);
        try {
            InputStream stream = context.getContentResolver().openInputStream(localContentUri);
            if (stream == null) {
                Log.w(TAG, "ContentResolver returned null stream for " + localContentUri);
                return localNotFoundResponse();
            }
            return new WebResourceResponse(mime, encoding, 200, "OK", headers, stream);
        } catch (Exception e) {
            Log.w(TAG, "Failed to open local document: " + localContentUri, e);
            return localNotFoundResponse();
        }
    }
    private void createWeb() {
        if (closed || container == null) return;
        if (localContentUri != null && localVirtualUri == null) {
            String displayName = resolveDisplayName(context, localContentUri);
            localDisplayName = displayName;
            localVirtualUri = new Uri.Builder()
                    .scheme("https")
                    .authority(localVirtualHost != null ? localVirtualHost : LOCAL_VIRTUAL_HOST)
                    .appendPath(displayName)
                    .build();
        }
        web = new WebView(presentation.getContext());
        web.setBackgroundColor(Color.TRANSPARENT);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.getSettings().setAllowFileAccess(allowFileAccess);
        web.getSettings().setAllowContentAccess(false);
        web.getSettings().setAllowFileAccessFromFileURLs(false);
        web.getSettings().setAllowUniversalAccessFromFileURLs(false);
        if (audioReroute) {
            web.getSettings().setMediaPlaybackRequiresUserGesture(false);
            web.addJavascriptInterface(new AudioBridge(), "__obsAudio");
        }
        web.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return request != null && isDisallowedLocalScheme(request.getUrl());
            }
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                if (closed || request == null)
                    return null;
                Uri reqUri = request.getUrl();
                if (isDisallowedLocalScheme(reqUri)) {
                    return emptyErrorResponse(403, "Forbidden");
                }
                if (localContentUri == null || localVirtualUri == null || localVirtualHost == null)
                    return null;
                if (reqUri == null || !"https".equalsIgnoreCase(reqUri.getScheme())
                        || !localVirtualHost.equalsIgnoreCase(reqUri.getHost())) {
                    return null;
                }
                String expectedPath = localVirtualUri.getPath();
                if (expectedPath != null && expectedPath.equals(reqUri.getPath())) {
                    return openLocalDocumentResponse();
                }
                return emptyErrorResponse(404, "Not Found");
            }
            @Override public void onPageStarted(WebView view, String pageUrl, Bitmap favicon) {
                injectAudioHook(view);
            }
            @Override public void onPageFinished(WebView view, String pageUrl) {
                if (closed || view != web) return;
                injectAudioHook(view);
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
        String loadTarget = localVirtualUri != null ? localVirtualUri.toString()
                : (url == null || url.isEmpty() ? "about:blank" : url);
        web.loadUrl(loadTarget);
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
    public int drainAudio(short[] out, int maxFrames, int[] outFormat) {
        synchronized (audioLock) {
            if (closed || out == null || maxFrames <= 0 || audioRing == null || audioFilled < audioChannels)
                return 0;
            final int availFrames = audioFilled / audioChannels;
            final int maxOutFrames = out.length / audioChannels;
            final int frames = Math.min(maxFrames, Math.min(availFrames, maxOutFrames));
            if (frames <= 0)
                return 0;
            final int total = frames * audioChannels;
            final int first = Math.min(total, audioRing.length - audioTail);
            System.arraycopy(audioRing, audioTail, out, 0, first);
            if (total > first)
                System.arraycopy(audioRing, 0, out, first, total - first);
            audioTail = (audioTail + total) % audioRing.length;
            audioFilled -= total;
            if (outFormat != null && outFormat.length >= 2) {
                outFormat[0] = audioRate;
                outFormat[1] = audioChannels;
            }
            return frames;
        }
    }
    /** No UI-thread wait: the producer is disconnected asynchronously. */
    public synchronized void close() {
        if (closed) return;
        closed = true;
        synchronized (audioLock) {
            audioHead = 0;
            audioTail = 0;
            audioFilled = 0;
            audioRing = null;
        }
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
