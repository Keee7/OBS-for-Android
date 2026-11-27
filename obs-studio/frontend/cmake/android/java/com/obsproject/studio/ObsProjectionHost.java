/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 屏幕采集授权与投影控制器宿主（plan.md §五 P-18-a）
 *
 * MediaProjection 不是可查权限（checkSelfPermission），而是一次性 token：系统只通过
 * onActivityResult 交付一次。ObsProjectionConsentActivity 作为透明代理 Activity 收取结果后
 * 立刻交回本类；本类统一维护令牌、VirtualDisplay 会话与诊断状态，供 native 与前台服务查询。
 *
 * 生命周期有两条硬规矩，两条都是量出来的（不是按记忆写的）：
 *   1) **一个进程换第二次令牌不会抛异常** —— 我原先记的是"第二次 getMediaProjection 抛
 *      IllegalStateException"，20:54:52 实测否掉了：第二次照样成功，代价是**系统当场收回第一份**
 *      （旧令牌的 onStop 在新令牌到手后 2 ms 到达）。⇒ 判空那层（requestConsent 里
 *      `projection != null` 直接返回 0）留着的理由是"别为同一件事问用户两次、别白换一次令牌"，
 *      不是"否则会抛"；而"换发时旧令牌的 onStop 不许碰新格子"由 takeToken 里那个身份判定兜
 *      （不判的话实测会把刚拿到的令牌擦掉）。
 *   2) **取令牌之前**就必须有一个 mediaProjection 档的前台服务在跑：只报 specialUse 时
 *      getMediaProjection 直接 SecurityException（原文与栈抄在 takeToken 上面）。
 *      于是流程是"同意 → 改服务档 → 才换令牌"，而不是我以为的"换令牌 → 改档 → 建 VD"。
 *      这条是量出来的：b109 失败（SecurityException）⇒ 改成先改档 ⇒ b110 在 20:48:57.883 拿到令牌。
 *      用户从系统通知里点"停止"时走 onStop 回调清格子。
 *
 * 线程：Activity 回调都在主线程；native 那边（Qt GUI 线程）调的静态方法全部 synchronized，
 * 且 requestConsent 不等框的结果（与相机总线"不得阻塞等用户点授权弹窗"同一条约定）。
 */
package com.obsproject.studio;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

public final class ObsProjectionHost {
    static final String TAG = "OBS-screen";
    static final String EXTRA_CONSENT_GENERATION = "consent_generation";

    /** 唯一的那份投影令牌。只在主线程写，别的线程读 —— volatile 够，因为没人拿它做复合判断。 */
    private static volatile MediaProjection projection;

    /**
     * "这一轮打算投影"的意图位。它和 projection 一起决定保活服务该报哪种前台类型，
     * 因为**这道门在取令牌那一步，不在建 VirtualDisplay**（b109 实测，见 takeToken 上方那段）。
     * 令牌要等前台服务已经带上 mediaProjection 那一档才要得下来，所以必须有一个"还没有令牌、
     * 但已经要那一档"的中间态 —— 这一格就是它。
     * 系统确认共享后才置位，停止投影或交换令牌失败时清除。
     */
    private static volatile boolean wantProjection;
    private static volatile boolean consentRequestPending;

    /** 同意框是系统那个 Activity，结果只给一次；令牌交换要在服务改档之后跑，所以管理器存静态。 */
    private static volatile MediaProjectionManager sMpm;

    /** 只给诊断用，形状与 ObsPermissionHost/ObsForegroundService 里那几个计数器一致 */
    private static volatile int consentRequests = 0;
    private static volatile int consentResults = 0;
    private static volatile int lastResultCode = -1;
    private static volatile String lastAction = "<none>";
    private static volatile String lastError = "<none>";

    /** Identifies the outstanding system prompt across Activity recreation. Guarded by the class monitor. */
    private static int consentGen = 0;
    private static long pendingActivityLaunchMs;

    private ObsProjectionHost() {}

    /* ===================== 给 native / 别的组件用的静态口 ===================== */

    /**
     * 起一次系统同意框。返回 0=令牌本来就有，1=已发起（结果异步，调用方之后再现读 hasProjection()），
     * -1=失败（拿不到 Context 或 startActivity 抛异常）。
     *
     * 这一格返回三态而不是布尔：libobs 那条总线的 obs_android_screen_request_consent() 就是三态，
     * 形状抄 ObsPermissionHost.requestCamera() —— 调用方（采集源的属性页）要能把"已经在路上"
     * 与"这机器上根本起不来"说成两句话。
     *
     * ctx 用 applicationContext 也够（不像 requestCamera 非要 Activity，因为这里不是
     * requestPermissions），所以补了 NEW_TASK。
     */
    public static synchronized int requestConsent(Context ctx) {
        if (projection != null) {
            lastAction = "已持有 MediaProjection 授权，无需重复请求";
            Log.i(TAG, lastAction);
            return 0;
        }
        long now = SystemClock.elapsedRealtime();
        // Android may drop a background Activity launch. Only expire requests
        // whose proxy never opened. A visible system prompt can wait indefinitely.
        if (consentRequestPending &&
            (pendingActivityLaunchMs == 0 || now - pendingActivityLaunchMs < 3000))
            return 1;
        if (ctx == null) {
            lastError = "requestConsent(null context)";
            Log.e(TAG, lastError);
            return -1;
        }
        consentRequestPending = true;
        pendingActivityLaunchMs = now;
        consentRequests++;
        try {
            Intent it = new Intent(ctx.getApplicationContext(), ObsProjectionConsentActivity.class);
            it.putExtra(EXTRA_CONSENT_GENERATION, ++consentGen);
            it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.getApplicationContext().startActivity(it);
            lastAction = "已启动授权代理 Activity，等待系统确认";
            Log.i(TAG, lastAction);
            return 1;
        } catch (Throwable t) {
            consentRequestPending = false;
            lastError = "startActivity(ObsProjectionConsentActivity) 抛异常: " + t;
            Log.e(TAG, lastError, t);
            return -1;
        }
    }

    /** 有没有可用的令牌 —— 这是"能不能建 VirtualDisplay"的唯一判据。 */
    public static synchronized boolean hasProjection() {
        return projection != null;
    }

    /** 交给 b 腿建 VirtualDisplay 与系统音频内录用。取走之后仍然由本类负责 stop。 */
    public static synchronized MediaProjection getProjection() {
        return projection;
    }

    static void recordAction(String action) {
        lastAction = action;
    }

    static void recordError(String error) {
        lastError = error;
    }

    /* ===================== P-18-b：把令牌接成一块投影面 ===================== */

    /**
     * 投影面与令牌同生同死，所以放在这个类里（放别处就会出现"令牌交还了、面还在"这种
     * 系统以为仍在录的态 —— 正是 P-18-d 要防的幽灵投影）。
     * Surface 是 native 那边用 ANativeWindow_toSurface 从 AImageReader 的窗口换来的，
     * 交进来之后由本类持有全局引用：VirtualDisplay 活着期间它不能被回收。
     */
    private static volatile ObsProjectionSession<Surface> session;
    private static volatile Surface captureSurface;

    private static android.view.Display defaultDisplay(Context ctx) {
        if (ctx == null)
            return null;
        final DisplayManager dm = (DisplayManager) ctx.getApplicationContext().getSystemService(
            Context.DISPLAY_SERVICE);
        return (dm == null) ? null : dm.getDisplay(android.view.Display.DEFAULT_DISPLAY);
    }

    /**
     * 一次量出 {宽, 高, dpi, 旋转档} 四格一起交出去。唯一的调用方是下面的 captureSize
     * （总线那一格 display_size 要的就是这一份），所以这里不拆成三个 getter。
     *
     * 为什么不能直接把 getMode() 的两个数交出去（b113 实测否掉了那一版）：
     *   21:32:46.285 那句"尺寸=成 720x1280@180"—— 号码对，方向反。getMode() 的
     *   getPhysicalWidth/Height 说的是**面板自然方向**（这台是 720x1280 竖屏面板），
     *   而我们跑横屏。照它建投影面就得到一块竖的 VD，症状是画面被拉伸/转 90°，
     *   不报错。⇒ 必须以 mode 为底、按 getRotation() 换位。
     * 为什么仍以 mode 为底、不整个换成 getRealSize：① mode 不受 `wm density` 覆盖影响，
     *   这正是当初不选 Qt 逻辑像素（1067x667）的同一条理由；② getRealSize 在 API 30 起被标废弃，
     *   拿它当唯一真相就是把自己绑在一个平台方明确说"别再用于排版"的口上。
     *   但它两个数到底一不一致，头里没说、我也不猜 —— 每次量取都顺手打一行对照
     *   （这一行就是这台机器上"旋转后 getRealSize 给什么"的读数，下一轮照它定要不要改底）。
     */
    private static int[] measureCapture(Context ctx) {
        final android.view.Display d = defaultDisplay(ctx);
        if (d == null) {
            lastError = "measureCapture：获取默认显示设备失败（DISPLAY_SERVICE 为空）";
            Log.e(TAG, lastError);
            return new int[] { 0, 0, 0, 0 };
        }

        int w = 0, h = 0, mw = 0, mh = 0, bwm = 0, bhm = 0;
        final int rot = d.getRotation();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            final android.view.Display.Mode m = d.getMode();
            if (m != null) {
                mw = m.getPhysicalWidth();
                mh = m.getPhysicalHeight();
            }
        }
        /* Display.Mode reports unrotated panel pixels and does not account for panel installOrientation
         * (common on tablets whose physical panel is portrait with ROTATION_90/270 installOrientation).
         * Display.getRealSize() reads DisplayInfo.logicalWidth/Height directly in the display's true
         * current orientation (unaffected by wm density or OBS's sensorLandscape activity orientation).
         * MaximumWindowMetrics (API 30+) is queried as fallback and for diagnostic comparison because
         * some Android 11-16 ROMs constrain ApplicationContext/DisplayContext WindowMetrics to the
         * app's locked orientation while OBS is in the background. */
        android.graphics.Point rs = new android.graphics.Point();
        d.getRealSize(rs);
        if (rs.x > 0 && rs.y > 0) {
            w = rs.x;
            h = rs.y;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                final Context displayCtx = ctx.getApplicationContext().createDisplayContext(d);
                final android.view.WindowManager wm =
                    (android.view.WindowManager) displayCtx.getSystemService(Context.WINDOW_SERVICE);
                if (wm != null) {
                    final android.graphics.Rect bounds = wm.getMaximumWindowMetrics().getBounds();
                    bwm = bounds.width();
                    bhm = bounds.height();
                    if ((w <= 0 || h <= 0) && bwm > 0 && bhm > 0) {
                        w = bwm;
                        h = bhm;
                    }
                }
            } catch (Throwable t) {
                Log.w(TAG, "getMaximumWindowMetrics 失败: " + t);
            }
        }
        if ((w <= 0 || h <= 0) && mw > 0 && mh > 0) {
            w = mw;
            h = mh;
            if (rot == android.view.Surface.ROTATION_90 || rot == android.view.Surface.ROTATION_270) {
                final int t = w;
                w = h;
                h = t;
            }
        }

        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        d.getRealMetrics(dm);
        /* 只在**读数变了**的时候打这一行（第一问必打）。d 腿起，采集源每秒问一次尺寸，
         * 无条件打就是一天两万行，而其中绝大多数与上一行一字不差。 */
        final int dpi = dm.densityDpi;
        if (w != lastMeasured[0] || h != lastMeasured[1] || dpi != lastMeasured[2] || rot != lastMeasured[3]) {
            lastMeasured = new int[] { w, h, dpi, rot };
            Log.i(TAG, "屏幕采集参数测量：maxMetrics=" + bwm + "x" + bhm + "，getRealSize=" + rs.x + "x" + rs.y
                               + "，mode=" + mw + "x" + mh + "，旋转=" + rot + " -> " + w + "x" + h + "@" + dpi + "dpi");
        }
        return new int[] { w, h, dpi, rot };
    }

    /** 上一次量到的 {宽, 高, dpi, 旋转档}，只给上面那行日志判"要不要说"。 */
    private static int[] lastMeasured = new int[4];

    /**
     * 一次 JNI 往返交出整份量取结果 {宽, 高, dpi, 旋转档}。
     *
     * 为什么不再拆成 captureWidth/Height/Dpi 三格（b 腿原本那三格）：d 腿的采集源要**每秒**问一次
     * 尺寸，好在旋转/分辨率变了的时候重建投影面 —— 三格就是三次完整量取，而每次量取都要打一行
     * "屏幕量取"日志（P-8 那条日志量的账正愁没处减）。一次往返、一份数组，问几轮都只量一次。
     */
    public static synchronized int[] captureSize(Context ctx) {
        final int[] displaySize = measureCapture(ctx);
        return session == null ? displaySize : session.captureSize(displaySize);
    }

    /**
     * Attach a capture surface, creating at most one mirrored display per consent.
     * On scene activation or rotation, reuse it via resize()/setSurface().
     * Failure details are available through captureError() and describe().
     */
    public static synchronized boolean startCapture(Context ctx, Surface s, int w, int h, int dpi) {
        if (projection == null) {
            lastError = "startCapture：没有令牌，建不了投影面（先走同意框）";
            Log.e(TAG, lastError);
            return false;
        }
        if (s == null || w <= 0 || h <= 0) {
            lastError = "startCapture：参数不成立 surface=" + s + " " + w + "x" + h;
            Log.e(TAG, lastError);
            return false;
        }
        if (captureSurface != null && captureSurface != s) {
            lastError = "已有屏幕源正在采集，请复用已有源，或停用它再启用另一屏幕源";
            return false;
        }
        try {
            // Android 14+: one createVirtualDisplay per consent, including scene reactivation.
            // Resizing/replacing an ImageReader must reuse that display.
            session.attach(s, w, h, (dpi > 0) ? dpi : 160);
            captureSurface = s;
        } catch (Throwable t) {
            final String error = "屏幕投影启动失败: " + t + "。请停用并重新启用屏幕源，重新授权共享。";
            lastError = error;
            Log.e(TAG, lastError, t);
            // A failed create may already consume the token. Never retry it in a loop.
            stopProjection("投影面创建或重接失败");
            lastError = error;
            return false;
        }
        lastError = "<none>";
        lastAction = "VirtualDisplay 已创建 " + w + "x" + h + "@" + dpi;
        Log.i(TAG, lastAction);
        return true;
    }

    /** Detach only the caller's surface, never a newer or unrelated capture. */
    public static synchronized void stopCaptureForSurface(Surface surface) {
        if (surface != null && captureSurface == surface)
            stopCapture("GPU surface closed");
    }

    /** Pause delivery while retaining the one display allocated to this consent. */
    public static synchronized void stopCapture(String why) {
        if (captureSurface == null)
            return;
        try {
            if (session != null) session.detach();
        } catch (Throwable t) {
            lastError = "VirtualDisplay.setSurface(null) 失败: " + t;
            Log.e(TAG, lastError, t);
            stopProjection("暂停投影面失败");
        }
        captureSurface = null;
        Log.i(TAG, "投影面已暂停，保留同一授权的显示（" + why + "）");
    }

    private static void releaseDisplay() {
        ObsProjectionSession<Surface> old = session;
        session = null;
        captureSurface = null;
        try {
            if (old != null) old.release();
        } catch (Throwable t) {
            Log.e(TAG, "释放投影显示失败", t);
        }
    }

    public static synchronized String captureError() {
        return lastError;
    }

    /** 投影面在不在（诊断用；真正的采集判据还是 hasProjection()）。 */
    public static synchronized boolean hasCapture() {
        return session != null && session.isAttached();
    }

    /**
     * 主动交还（P-18-d：最后一颗采集源销毁时 native 侧那一格走这里）。
     * **幂等**：手里既没令牌又没意图位就直接返回 false —— 那不是失败，是"没有可交的东西"，
     * 所以调用方（前端那一格）只在返回 true 的那一发记一行日志。重复调同样安全。
     */
    public static synchronized boolean stopProjection(String why) {
        if (projection == null && !wantProjection && !consentRequestPending)
            return false;
        ++consentGen;
        consentRequestPending = false;
        releaseDisplay();
        if (projection != null) {
            try {
                projection.stop();
            } catch (Throwable t) {
                lastError = "MediaProjection.stop() 抛异常: " + t;
                Log.e(TAG, lastError, t);
            }
        }
        projection = null;
        wantProjection = false; // 意图位跟着一起落，否则保活服务会一直白占 mediaProjection 那一档
        lastAction = "MediaProjection 投影已释放（" + why + "）";
        Log.i(TAG, lastAction);
        ObsForegroundService.retypeIfNeeded(); // 前台服务里 mediaProjection 那一位要跟着落
        return true;
    }

    /**
     * 保活服务挑前台类型时问的就是这一格：**"打算投影"或"已经握着令牌"**都算要 mediaProjection 那一档。
     * 只用 hasProjection() 会死锁在顺序上 —— 令牌要等服务改好档才要得下来（见 takeToken 上面那段实测）。
     */
    public static synchronized boolean projectionActive() {
        return wantProjection || projection != null;
    }

    /** 一行状态快照，进 native 的 blog。 */
    public static synchronized String describe() {
        /* resultCode 要翻成人话再打：RESULT_OK 的字面值就是 -1，而 lastResultCode 的初值也是 -1，
         * 只印数字的话"用户点了同意"与"还没收到过结果"长得一模一样（20:48 那次实测的日志里就是
         * "最后一次 resultCode=-1"配着"令牌=有"，读的人得反过来推一遍才敢信）。 */
        String verdict;
        if (consentResults == 0)
            verdict = "未收到结果";
        else if (lastResultCode == Activity.RESULT_OK)
            verdict = "RESULT_OK";
        else if (lastResultCode == Activity.RESULT_CANCELED)
            verdict = "RESULT_CANCELED";
        else
            verdict = "其它";
        return "令牌=" + (projection != null ? "有" : "无") + " 意图=" + (wantProjection ? "要投影" : "无")
               + " 面=" + (session != null && session.hasDisplay() ? (session.isAttached() ? "已建" : "暂停") : "无") + " 发起=" + consentRequests + " 收到结果=" + consentResults
               + " 最后一次 resultCode=" + verdict + "(" + lastResultCode + ")"
               + " 动作=" + lastAction + " 错误=" + lastError;
    }

    /* ===================== 透明授权代理 Activity 回调 coordination ===================== */

    static synchronized boolean beginConsentActivity(int generation) {
        if (consentGen == 0 && generation != 0) {
            consentGen = generation;
            consentRequestPending = true;
        }
        if (generation != consentGen || !consentRequestPending || projection != null)
            return false;
        pendingActivityLaunchMs = 0;
        return true;
    }

    static synchronized MediaProjectionManager acquireProjectionManager(Context app, int generation) {
        /* 用 applicationContext 拿管理器：代理 Activity 收完结果就 finish，
         * 而令牌交换排在服务改档之后（那一刻实例可能已经离场），静态字段不能握着 Activity 的 Context。 */
        sMpm = (MediaProjectionManager) app.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (sMpm == null) {
            lastError = "获取 MediaProjectionManager 失败，当前设备可能不支持屏幕采集";
            abortConsent(generation);
            Log.e(TAG, lastError);
            return null;
        }
        return sMpm;
    }

    static synchronized void onConsentLaunchFailed(int generation, Throwable t) {
        lastError = "发起系统录屏授权请求时发生异常: " + t;
        abortConsent(generation);
        Log.e(TAG, lastError, t);
    }

    static synchronized void onConsentActivityResult(Context app, int generation, int resultCode, Intent data) {
        if (generation != consentGen || !consentRequestPending || wantProjection)
            return;
        consentResults++;
        lastResultCode = resultCode;
        if (resultCode != Activity.RESULT_OK || data == null) {
            lastAction = "用户取消或拒绝了屏幕采集授权";
            Log.i(TAG, lastAction);
            consentRequestFailed();
            return;
        }
        wantProjection = true;
        // Android requires this foreground service type before exchanging the result for a token.
        Log.i(TAG, "已获取屏幕录制授权，正在更新前台服务类型以创建投影对象");
        ObsForegroundService.ensureProjectionForeground(app, () -> takeToken(app, resultCode, data, generation));
    }

    /**
     * b109 实测到的顺序约束（09-07 20:29:32.468，Android 15 / API 35，MuMu）：
     * 服务只报 specialUse 时直接 getMediaProjection 会抛
     *   java.lang.SecurityException: Media projections require a foreground service of type
     *   ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
     * 栈是 IMediaProjection$Stub$Proxy.start() ← MediaProjection.&lt;init&gt; ←
     * MediaProjectionManager.getMediaProjection() ← 本类 takeToken。
     * ⇒ 这道门在**取令牌**那一步，不在建 VirtualDisplay。我在头注释与
     * ObsForegroundService 头注释里按记忆写的都是后者，两处已按这条实测改掉。
     */
    private static synchronized void takeToken(Context app, int resultCode, Intent data, int generation) {
        if (generation != consentGen || !consentRequestPending)
            return;
        try {
            MediaProjection p = sMpm.getMediaProjection(resultCode, data);
            if (p == null) {
                lastError = "getMediaProjection 返回 null";
                Log.e(TAG, lastError);
                giveUpToken(app, "getMediaProjection 返回 null");
                return;
            }
            /* 这份回调只认自己那一份令牌。换发新令牌时系统会先收回旧的（20:54:52.132 实测：
             * 第二次 getMediaProjection 成功之后 2 ms，旧令牌的 onStop 到了），而那一刻格子已经
             * 指向新的 —— 不判身份就会把刚拿到的令牌一起擦掉：实测的后果是
             * dumpsys media_projection 仍登记着我们的 TYPE_SCREEN_CAPTURE、hasProjection() 却报无、
             * 前台类型顺带落回 specialUse（0x40000020 → 0x40000000），正好是 Android 14+ 禁止的那个态。 */
            final MediaProjection mine = p;
            p.registerCallback(new MediaProjection.Callback() {
                @Override
                public void onCapturedContentResize(int width, int height) {
                    synchronized (ObsProjectionHost.class) {
                        if (projection != mine || session == null) return;
                        if (session.contentResized(width, height))
                            Log.i(TAG, "采集内容尺寸=" + width + "x" + height +
                                "；采集源将按内容比例重接现有投影显示");
                    }
                    // Native polls captureSize(). It replaces its Surface and reuses the
                    // same VirtualDisplay, applying the user's 1080p/native size limit.
                }

                @Override
                public void onStop() {
                    final boolean current;
                    final String line;
                    synchronized (ObsProjectionHost.class) {
                        current = (projection == mine);
                        if (current) {
                            releaseDisplay();
                            projection = null;
                            wantProjection = false;
                            lastAction = "系统已停止 MediaProjection 会话（onStop）";
                            line = lastAction;
                        } else {
                            line = "这份 onStop 不是现在手里那一份令牌 ⇒ 忽略，不动现在的态"
                                   + "（两种来源：换发时被系统顶掉的旧令牌，或我们主动交还、格子已清空的那一份）";
                        }
                    }
                    if (current) {
                        Log.w(TAG, line);
                        ObsForegroundService.retypeIfNeeded(); // 只有真的收回了当前令牌才要落那一档
                    } else {
                        Log.i(TAG, line);
                    }
                }
            }, null);
            final boolean recreateOnResize = Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE;
            session = new ObsProjectionSession<>((surface, width, height, density) -> {
                final VirtualDisplay vd = mine.createVirtualDisplay("obs-screen", width, height, density,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null);
                if (vd == null) return null;
                return new ObsProjectionSession.Display<Surface>() {
                    public void resize(int w, int h, int dpi) { vd.resize(w, h, dpi); }
                    public void surface(Surface target) { vd.setSurface(target); }
                    public void release() { vd.release(); }
                };
            }, recreateOnResize);
            projection = p;
            consentRequestPending = false;
            lastAction = "已获取 MediaProjection 授权（API " + Build.VERSION.SDK_INT + "）";
            Log.i(TAG, lastAction + " " + describe());
            /* 令牌到手，"为投影空手起服务"这面旗子的使命到此为止。不落回 false 的话，
             * 之后任何一次投影失败收尾都会把整个保活服务停掉 —— 那时若正在推流/录制，
             * 等于被投影那条腿顺手拆了保活（见 ObsForegroundService.projectionEstablished）。 */
            ObsForegroundService.projectionEstablished();
        } catch (Throwable t) {
            lastError = "取令牌/registerCallback 抛异常: " + t;
            Log.e(TAG, lastError, t);
            giveUpToken(app, "取令牌抛异常");
        }
    }

    /** 同意已经给了、令牌却没拿到：把意图位落回去，并把**我们替它起起来的那个服务**收掉。 */
    private static synchronized void giveUpToken(Context app, String why) {
        projection = null;
        wantProjection = false;
        Log.w(TAG, "屏幕投影已中止（" + why + "），已更新前台服务状态");
        ObsForegroundService.releaseProjectionForeground(app);
    }

    private static void consentRequestFailed() {
        wantProjection = false;
        consentRequestPending = false;
    }

    private static synchronized void abortConsent(int generation) {
        if (generation == consentGen)
            consentRequestFailed();
    }

    static void projectionForegroundFailed() {
        if (wantProjection && consentRequestPending)
            consentRequestFailed();
    }
}
