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

public final class ObsProjectionAudio {
    private static final String TAG = ObsProjectionHost.TAG;

    /** 非 null 就代表"正在录"，同时也是 start 幂等的那一格。 */
    private static volatile AudioRecord audioRecord;
    private static volatile boolean audioRunning;
    private static Thread audioThread;
    private static volatile int audioRate, audioChannels;

    private ObsProjectionAudio() {}

    private static native void nativeOnPcm(short[] pcm, int frames, int sampleRate, int channels);

    /**
     * 起内录。成功返回 {实到采样率, 实到声道数}，失败返回 null（原因留在 ObsProjectionHost 诊断里）。
     * 返回那两个数而不是写出参，是为了让前端一次 JNI 往返拿全。
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
            AudioRecord rec = null;
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
                return null;
            }

            /* 实到的那一组才是真相：AudioRecord 允许把请求的格式换成它自己的。 */
            audioRate = rec.getSampleRate();
            audioChannels = rec.getChannelCount();
            if (audioRate <= 0 || audioChannels <= 0) {
                final String err = "startAudio：实际音频格式参数无效，rate=" + audioRate + " ch=" + audioChannels;
                ObsProjectionHost.recordError(err);
                Log.e(TAG, err);
                rec.release();
                return null;
            }
            audioRecord = rec;
            audioRunning = true;
            final AudioRecord started = rec;
            final int sampleRate = audioRate;
            final int channels = audioChannels;
            audioThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    audioLoop(started, sampleRate, channels);
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

    /** 读线程的体：不持类锁（stop 要能进来 join），每次 read 后直接推给 native。 */
    private static void audioLoop(AudioRecord rec, int rate, int channels) {
        final int maxFrames = Math.max(1, 8192 / channels);
        final int chunkFrames = Math.min(Math.max(1, rec.getBufferSizeInFrames()), maxFrames);
        final short[] buf = new short[chunkFrames * channels];
        while (audioRunning) {
            final int n = rec.read(buf, 0, buf.length);
            if (n == 0)
                continue;
            if (n < 0) {
                /* ERROR_DEAD_OBJECT 与"令牌没了"同义；这里只停自己这一圈，
                 * 收尾留给 stop（native 侧发现令牌走了会调它）。 */
                Log.w(TAG, "内录读取线程返回 read()=" + n + "，退出读取循环");
                break;
            }
            final int frames = n / channels;
            if (frames > 0)
                nativeOnPcm(buf, frames, rate, channels);
        }
        audioRunning = false;
    }

    /** 停内录。幂等，且返回时读线程一定已经退出，确保 native 回调不再进入。 */
    public static synchronized void stop() {
        if (audioRecord == null && audioThread == null)
            return;
        audioRunning = false;
        final AudioRecord rec = audioRecord;
        audioRecord = null;
        /* 先 stop、再 join：AudioRecord.stop() 会让阻塞中的 read() 立刻返回，join 一般几毫秒就回。 */
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
        if (rec != null) {
            try {
                rec.release();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.release() 发生异常: " + e, e);
            }
        }
        Log.i(TAG, "系统内录已停止");
    }
}
