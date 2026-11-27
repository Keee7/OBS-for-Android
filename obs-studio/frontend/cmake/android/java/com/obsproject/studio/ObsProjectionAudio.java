/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * System audio playback capture (AudioPlaybackCapture) reader attached to the
 * active MediaProjection token managed by ObsProjectionHost.
 *
 * Java 的读线程从 AudioRecord 读到 PCM 后通过 nativeOnPcm 直接推入 native 回调
 * （OBSAndroidPermissions.cpp 注册），省去环形缓冲与 C 侧 10ms 轮询线程。
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
import java.util.concurrent.atomic.AtomicBoolean;

public final class ObsProjectionAudio {
    private static final String TAG = ObsProjectionHost.TAG;

    /**
     * 单次采集会话：运行标志与会话 ID 绑定到本次实例，避免 join(2000) 超时后
     * 旧读线程把数据推给新会话，或在退出时误清全局运行标志杀掉新会话。
     */
    private static final class Session {
        volatile long id;
        final AudioRecord record;
        final int rate;
        final int channels;
        final AtomicBoolean running = new AtomicBoolean(true);
        Thread thread;

        Session(long id, AudioRecord record, int rate, int channels) {
            this.id = id;
            this.record = record;
            this.rate = rate;
            this.channels = channels;
        }

        boolean isAlive() {
            return running.get() && thread != null && thread.isAlive();
        }
    }

    private static volatile Session activeSession;

    private ObsProjectionAudio() {}

    private static native void nativeOnPcm(long sessionId, short[] pcm, int frames, int sampleRate, int channels);

    /**
     * 起内录。成功返回 {实到采样率, 实到声道数}，失败返回 null（原因留在 ObsProjectionHost 诊断里）。
     * sessionId 由 native 在启动前分配并传入，确保首帧到达时即可完成会话校验。
     */
    public static synchronized int[] start(long sessionId) {
        final Session existing = activeSession;
        if (existing != null) {
            if (existing.isAlive()) {
                /* 幂等：当前读线程仍存活时仅更新会话 ID，不重复起线程。 */
                existing.id = sessionId;
                return new int[] { existing.rate, existing.channels };
            }
            /* 旧读线程已异常退出（如 ERROR_DEAD_OBJECT）：先释放旧 AudioRecord 再重建。 */
            stop();
        }
        final MediaProjection projection = ObsProjectionHost.getProjection();
        if (projection == null) {
            final String err = "startAudio：缺少 MediaProjection 授权令牌，无法启动系统内录";
            ObsProjectionHost.recordError(err);
            Log.e(TAG, err);
            return null;
        }
        AudioRecord rec = null;
        try {
            /* 一条规则都不加是建不起来的（Cannot build AudioMixingRule with no rules）；
             * MEDIA / GAME / UNKNOWN 是系统允许内录的三类 usage 上限。 */
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
            /* 系统可能拒绝首选采样率组合（模拟器与部分机型上 48k 输入档不存在），
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
            if (rec.getRecordingState() == AudioRecord.RECORDSTATE_STOPPED)
                rec.startRecording();
            if (rec.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                final String err = "startAudio：AudioRecord 创建成功但未能进入 RECORDING 状态（rate=" + rec.getSampleRate() +
                                   " ch=" + rec.getChannelCount() + " state=" + rec.getState() + "）";
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                rec.release();
                rec = null;
                return null;
            }

            /* 实到的那一组才是真相：AudioRecord 允许把请求的格式换成它自己的。 */
            final int sampleRate = rec.getSampleRate();
            final int channels = rec.getChannelCount();
            if (sampleRate <= 0 || channels <= 0) {
                final String err = "startAudio：实际音频格式参数无效，rate=" + sampleRate + " ch=" + channels;
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                rec.release();
                rec = null;
                return null;
            }
            final Session session = new Session(sessionId, rec, sampleRate, channels);
            session.thread = new Thread(new Runnable() {
                @Override
                public void run() {
                    audioLoop(session);
                }
            }, "obs-screen-audio");
            activeSession = session;
            rec = null; /* 所有权已移交 session */
            final String action = "系统内录已启动（" + sampleRate + "Hz, " + channels + "ch）";
            ObsProjectionHost.recordAction(action);
            Log.i(TAG, action);
            session.thread.start();
            return new int[] { sampleRate, channels };
        } catch (Throwable t) {
            final String err = "startAudio 抛异常: " + t;
            ObsProjectionHost.recordError(err);
            Log.e(TAG, err, t);
            if (rec != null) {
                try {
                    rec.release();
                } catch (Throwable ignored) {
                }
            }
            final Session s = activeSession;
            if (s != null && s.id == sessionId) {
                s.running.set(false);
                activeSession = null;
                try {
                    s.record.release();
                } catch (Throwable ignored) {
                }
            }
            return null;
        }
    }

    /** 读线程的体：不持类锁（stop 要能进来 join），每次 read 后直接推给 native。 */
    private static void audioLoop(Session s) {
        final int maxFrames = Math.max(1, 8192 / s.channels);
        final int chunkFrames = Math.min(Math.max(1, s.record.getBufferSizeInFrames()), maxFrames);
        final short[] buf = new short[chunkFrames * s.channels];
        try {
            while (s.running.get()) {
                final int n = s.record.read(buf, 0, buf.length);
                if (!s.running.get())
                    break;
                if (n == 0)
                    continue;
                if (n < 0) {
                    /* ERROR_DEAD_OBJECT 与"令牌没了"同义；这里只停自己这一圈，
                     * 收尾留给 stop（native 侧发现令牌走了会调它）。 */
                    Log.w(TAG, "内录读取线程返回 read()=" + n + "，退出读取循环");
                    break;
                }
                final int frames = n / s.channels;
                if (frames > 0)
                    nativeOnPcm(s.id, buf, frames, s.rate, s.channels);
            }
        } finally {
            s.running.set(false);
        }
    }

    /** 停内录。幂等，且返回时读线程一定已经退出，确保 native 回调不再进入。 */
    public static synchronized void stop() {
        final Session s = activeSession;
        if (s == null)
            return;
        activeSession = null;
        s.running.set(false);
        /* 先 stop、再 join：AudioRecord.stop() 会让阻塞中的 read() 立刻返回，join 一般几毫秒就回。 */
        if (s.record != null) {
            try {
                if (s.record.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING)
                    s.record.stop();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.stop() 发生异常: " + e, e);
            }
        }
        final Thread t = s.thread;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (t.isAlive())
                Log.w(TAG, "内录读取线程在 join(2000) 超时后仍未退出，将继续释放 AudioRecord");
        }
        if (s.record != null) {
            try {
                s.record.release();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.release() 发生异常: " + e, e);
            }
        }
        Log.i(TAG, "系统内录已停止");
    }
}
