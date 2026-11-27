/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * System audio playback capture (AudioPlaybackCapture) ring buffer and reader thread
 * attached to the active MediaProjection token managed by ObsProjectionHost.
 *
 * 形状与视频那一路刻意相反：**这里 Java 只攒不推**。Java 侧起一条读线程把 AudioRecord
 * 读出来的 PCM 写进环形缓冲，native（插件自己那条音频线程）按帧来取 —— 全树至今没有
 * 一条 Java→native 的 JNI 导出，为了音频开第一条不值当（理由抄在 obs-android.h 那三格上方）。
 *
 * 令牌与音频的关系：AudioPlaybackCapture 依附在同一份 MediaProjection 上，令牌一走录音就废，
 * 所以 start 先判 getProjection()==null；而"令牌被系统收回"那一发（onStop）里我们**不**去
 * 直接停这条线程 —— 那是 native 的生命周期（它先发现令牌没了，会照视频那一路一起 stop_audio），
 * 在回调线程里 join 一条线程只会把主线程搭进去。
 */
package com.obsproject.studio;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.projection.MediaProjection;
import android.util.Log;

public final class ObsProjectionAudio {
    private static final String TAG = ObsProjectionHost.TAG;
    private static final Object AUDIO_LOCK = new Object();

    /** 环形缓冲：交织 int16（与 AudioRecord.read 的排布一致，插件那侧也按交织收）。 */
    private static short[] audioRing;
    private static int audioHead, audioTail, audioFilled, audioDropped;

    /** 非 null 就代表"正在录"，同时也是 start 幂等的那一格。 */
    private static volatile AudioRecord audioRecord;
    private static volatile boolean audioRunning;
    private static Thread audioThread;
    private static volatile int audioRate, audioChannels;

    private ObsProjectionAudio() {}

    /**
     * 起内录。成功返回 {实到采样率, 实声道数}，失败返回 null（原因留在 ObsProjectionHost 诊断里）。
     * 返回那两个数而不是写出参，是为了让前端像 captureSize 那样**一趟 JNI** 拿全（见那边注释）。
     */
    public static synchronized int[] start() {
        if (audioRecord != null) {
            /* 幂等：第二颗源（或重建后 native 又问一次）不该再起一条线程。 */
            return new int[] { audioRate, audioChannels };
        }
        final MediaProjection projection = ObsProjectionHost.getProjection();
        if (projection == null) {
            final String err = "startAudio：缺少 MediaProjection 授权令牌，无法启动系统内录";
            ObsProjectionHost.recordError(err);
            Log.e(TAG, err);
            return null;
        }
        try {
            /* 一条规则都不加是**建不起来**的，不是"全收"：这一发在设备上量到
             * java.lang.IllegalArgumentException: Cannot build AudioMixingRule with no rules.
             * （AudioMixingRule 的 build 就要至少一条 match 规则）。官方示例的那三条可内录 usage
             * 就是这三条：MEDIA / GAME / UNKNOWN —— usage 维度只列这三个是**系统规定**的上限
             * （通话、无障碍提示音、助手回传那几类本来就禁止内录，加进去也不会多收到东西）。 */
            final AudioPlaybackCaptureConfiguration capCfg = new AudioPlaybackCaptureConfiguration.Builder(
                                                               projection)
                                                               .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                                                               .addMatchingUsage(AudioAttributes.USAGE_GAME)
                                                               .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                                                               .build();

            int rate = AudioTrack.getNativeOutputSampleRate(AudioManager.USE_DEFAULT_STREAM_TYPE);
            if (rate <= 0)
                rate = 48000;
            int minBuf = -1;
            AudioRecord rec = null;
            /* 系统可能拒绝这个采样率组合（模拟器与部分机型上 48k 输入档不存在），
             * 44.1k 兜一遍；两回都失败就如实报，不猜第三个。 */
            for (final int tryRate : new int[] { rate, 44100 }) {
                minBuf = AudioRecord.getMinBufferSize(tryRate, AudioFormat.CHANNEL_IN_STEREO,
                                                      AudioFormat.ENCODING_PCM_16BIT);
                if (minBuf <= 0)
                    continue;
                final AudioFormat fmt = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                              .setSampleRate(tryRate)
                                              .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                                              .build();
                /* 口名是 javap 现查的，不是照印象写的：这里真叫 **setAudioPlaybackCaptureConfig**，
                 * 而文档与各处示例里常见的 setPlaybackCaptureConfiguration 在这个 android.jar（API 35）
                 * 里根本不存在（编译器当场否掉了那一发）。这类"凭记忆写 API 形状"的坑已经踩过不止一次，
                 * 查一次 20 秒，编过再写下来。 */
                rec = new AudioRecord.Builder().setAudioFormat(fmt).setAudioPlaybackCaptureConfig(capCfg).build();
                if (rec != null && rec.getState() == AudioRecord.STATE_INITIALIZED) {
                    rate = tryRate;
                    break;
                }
                if (rec != null)
                    rec.release();
                rec = null;
            }
            if (rec == null) {
                final String err = "startAudio：AudioRecord 创建失败（48k/44.1k 采样率均初始化失败，最终 getMinBufferSize=" +
                                   minBuf + "）";
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                return null;
            }
            /* 判据别写反：刚 build 出来的 AudioRecord 就是 STOPPED 态，所以"没在录"要写成
             * == STOPPED。上一版写的是 != STOPPED ⇒ startRecording() 一次都没调过，
             * 当场量到"建起来了但没进 RECORDING 态"（r_submix 那条输入管道是构造时自动开的，
             * 不是 start 开的，所以日志看着像开了 —— 别拿它当证据）。 */
            if (rec.getRecordingState() == AudioRecord.RECORDSTATE_STOPPED)
                rec.startRecording();
            if (rec.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                final String err = "startAudio：AudioRecord 创建成功但未能进入 RECORDING 状态（rate=" + rec.getSampleRate() +
                                   " ch=" + rec.getChannelCount() + " state=" + rec.getState() + "）";
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                rec.release();
                return null;
            }

            /* 实到的那一组才是真相：AudioRecord 允许把我们要求的格式换成它自己的。
             * 报错了就是变速/变调，所以出参、缓冲尺寸、drain 的换算全部以这两个为准。 */
            audioRate = rec.getSampleRate();
            audioChannels = rec.getChannelCount();
            if (audioRate <= 0 || audioChannels <= 0) {
                final String err = "startAudio：实际音频格式参数无效，rate=" + audioRate + " ch=" + audioChannels;
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                rec.release();
                return null;
            }
            /* 环形缓冲给 500 ms：native 那条线程取空就睡 10 ms，正常用不到十分之一；
             * 真填满时是消费方卡了，那时候丢旧留新（保持"实时"），并计数报出来。 */
            synchronized (AUDIO_LOCK) {
                audioRing = new short[500 * audioRate / 1000 * audioChannels];
                audioHead = audioTail = audioFilled = audioDropped = 0;
            }
            audioRecord = rec;
            audioRunning = true;
            /* rec 在上面的候选档循环里赋过值，不是 effectively final ⇒ 匿名类里引用不了它（编译器当场否的）。
             * 抄一枚 final 进去，顺带把"读线程手里那个实例就是这一发建起来的"说死。 */
            final AudioRecord started = rec;
            audioThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    audioLoop(started);
                }
            }, "obs-screen-audio");
            audioThread.start();
            final String action = "系统内录已启动（" + audioRate + "Hz, " + audioChannels + "ch）";
            ObsProjectionHost.recordAction(action);
            Log.i(TAG, action);
            return new int[] { audioRate, audioChannels };
        } catch (Throwable t) {
            final String err = "startAudio 抛异常: " + t;
            ObsProjectionHost.recordError(err);
            Log.e(TAG, err, t);
            audioRecord = null;
            audioRunning = false;
            return null;
        }
    }

    /** 读线程的体。不持类锁（stop 要能进来 join），只在 AUDIO_LOCK 里碰环形缓冲。 */
    private static void audioLoop(AudioRecord rec) {
        final int chunk = Math.max(1, rec.getBufferSizeInFrames()) * audioChannels;
        final short[] buf = new short[Math.min(chunk, 8192)];
        while (audioRunning) {
            final int n = rec.read(buf, 0, buf.length);
            if (n == 0)
                continue;
            if (n < 0) {
                /* ERROR_DEAD_OBJECT 与"令牌没了"同义；这里只停自己这一圈，
                 * 格子的清理留给 stop（native 那一侧发现令牌走了会调它）。 */
                Log.w(TAG, "内录读取线程返回 read()=" + n + "，退出读取循环");
                break;
            }
            synchronized (AUDIO_LOCK) {
                if (audioRing == null)
                    break;
                int free = audioRing.length - audioFilled;
                if (free < n) {
                    final int drop = n - free;
                    audioTail = (audioTail + drop) % audioRing.length;
                    audioFilled -= drop;
                    audioDropped += drop;
                    if ((audioDropped % (audioRate * audioChannels)) < n)
                        Log.w(TAG, "内录环形缓冲区溢出，已丢弃较旧采样累计 " + audioDropped + " samples（消费速率过慢）");
                }
                final int first = Math.min(n, audioRing.length - audioHead);
                System.arraycopy(buf, 0, audioRing, audioHead, first);
                if (first < n)
                    System.arraycopy(buf, first, audioRing, 0, n - first);
                audioHead = (audioHead + n) % audioRing.length;
                audioFilled += Math.min(n, audioRing.length - audioFilled);
            }
        }
        audioRunning = false;
    }

    /** 停内录。**幂等**，且返回时读线程一定已经不在（native 紧接着要拆它自己的缓冲）。 */
    public static synchronized void stop() {
        if (audioRecord == null && audioThread == null)
            return;
        audioRunning = false;
        final AudioRecord rec = audioRecord;
        audioRecord = null;
        /* 顺序是这一格的要点：**先 stop、再 join**。读线程平时正卡在 rec.read() 里等数据，
         * 而那一下是阻塞的 —— 只翻 audioRunning 不会把它叫醒，join(2000) 只会白等满两秒，
         * 然后我们在"线程还活着、可能正从缓冲里读"的时候去 release，正是注释里那句
         * "故意不制造边读边拆"要防的事。AudioRecord.stop() 会让卡住的那次 read() 立刻返回
         * （通常带 ERROR_INVALID_OPERATION），循环这才看得到 audioRunning==false 而退出；
         * 先 stop 之后 join 一般几毫秒就回。
         */
        if (rec != null) {
            try {
                if (rec.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING)
                    rec.stop();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.stop() 发生异常: " + e, e);
            }
        }
        final Thread t = audioThread;
        audioThread = null;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (t.isAlive())
                Log.w(TAG, "内录读取线程在 join(2000) 超时后仍未退出，将继续释放 AudioRecord");
        }
        synchronized (AUDIO_LOCK) {
            /* 放在 join 之后：读线程退出前还要读 audioRing，先清它就得靠 audioRing==null
             * 那条 break 兜底（audioLoop 里确有）。join 超时那条路径正是靠它收尾。 */
            audioRing = null;
            audioHead = audioTail = audioFilled = 0;
        }
        if (rec != null) {
            try {
                rec.release();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.release() 发生异常: " + e, e);
            }
        }
        Log.i(TAG, "系统内录已停止（累计丢弃 " + audioDropped + " samples）");
    }

    /**
     * 取走最多 maxFrames 帧（交织 int16），返回**实际帧数**；0 = 此刻没有，不是错误。
     * out 由前端复用同一枚数组（不每次新建，那是每秒几十次的垃圾），长度已按声道数算好。
     */
    public static int drain(short[] out, int maxFrames) {
        final int ch = audioChannels;
        if (ch <= 0 || out == null || maxFrames <= 0)
            return 0;
        final int wantShorts = Math.min(maxFrames, out.length / ch) * ch;
        synchronized (AUDIO_LOCK) {
            if (audioRing == null || audioFilled <= 0 || wantShorts <= 0)
                return 0;
            final int n = Math.min(wantShorts, audioFilled);
            final int first = Math.min(n, audioRing.length - audioTail);
            System.arraycopy(audioRing, audioTail, out, 0, first);
            if (first < n)
                System.arraycopy(audioRing, 0, out, first, n - first);
            audioTail = (audioTail + n) % audioRing.length;
            audioFilled -= n;
            return n / ch;
        }
    }
}
