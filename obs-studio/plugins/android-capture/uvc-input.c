/* SPDX-License-Identifier: GPL-2.0-or-later */
// UVC 采集源（USB Webcam / HDMI 采集卡，自写最小 UVC-over-libusb，无 root）。
//
// 为什么是重写而不是继续用 libuvc：旧实现经 libuvc 的 uvc_init(&ctx, NULL) 起内部
// libusb 上下文 —— 那一步在 Android 上必挂（libusb 默认初始化要建 NETLINK_KOBJECT_UEVENT
// 热插拔 socket，SELinux 直接拒，真机实测 -1/LIBUSB_ERROR_IO）。nikon-source 已真机验证
// 唯一可行的模式：libusb_init_context(LIBUSB_OPTION_NO_DEVICE_DISCOVERY) +
// libusb_wrap_sys_device(fd)。libuvc 0.0.7 不允许把自建上下文喂给 uvc_init 还让它泵事件
// （own_usb_ctx=0 就不起事件线程，回调永远不来），所以这条路只能自己写。
//
// 第二道坎：平板的内核 uvcvideo 驱动会抢先绑定采集卡（出现 /dev/videoN），claim 接口
// 返回 LIBUSB_ERROR_BUSY。Camera2 没注册外接摄像头、应用没 root 读不了 /dev/videoN，
// 所以唯一的活路是 claim 前 detach 内核驱动（本机 ROM 实测放行 USBDEVFS_DISCONNECT，
// 但代码仍按"尽力而为 + 清晰日志"写：set_auto_detach + 显式 detach，失败照记不误）。
//
// 第三道坎（真机第二、五轮测出来的）：这类采集卡**不把每个 UVC payload 用短包收尾**，
// 且 **payload 数据长度不固定** —— 每帧用一个 0 字节 payload 带 BMH EOF 位收尾（hex dump
// 实锤：12 字节头 0c 8e/0c 8d + JPEG 数据），帧内各块长度也未必一致。所以一切
// 定长/等距假设都不成立，解析器是纯字节扫描状态机：扫下一个合法头（HLE 白名单 +
// BMH EOH/保留位校验），两个头之间的字节全部归前一个头的 payload；帧边界只信 EOF；
// 误同步防护 = 帧内所有头的 PTS 必须一致（随机数据撞 PTS 约 2^-32）+ FID 一致。
// 送帧校验：MJPEG 必须 FFD8 开头、FFD9 在尾部填充窗内；未压缩必须够定长。
//
// 拔插健壮性（真机第四轮）：bulk/控制传输见 -1(IO)/-4(NO_DEVICE) 立刻拆会话重枚举；
// 设备匹配优先路径字符串，失配按 vid:pid（label 前缀）认同一台换号重插的设备；
// 流停滞有 clear_halt → 重新协商 → reset_device → 拆会话 的看门狗阶梯；协商侧
// GET probe 失败先重试、再跳过 readback 直接 SET+commit、最后 SET_INTERFACE 踢一脚。
//
// 拓扑假设（按 UGREEN 95348 / MS2130 类采集卡实测，也覆盖普通 UVC 摄像头）：
//   接口 N   = VideoControl（EP0 走 probe/commit，不需要 claim）
//   接口 N+1 = VideoStreaming，bulk IN 端点（MS2130 无等时端点，不需要 alt-setting 带宽协商）
// 描述符解析、probe/commit 协商、payload 重组都是按 UVC 1.0/1.1 规范自写的最小实现，
// MJPEG 由 turbojpeg 解码。未压缩格式直接传递给 OBS，RGB24 按需扩展。
//
// fd 从哪来：与 nikon-source 同一条路 —— Java 侧 UsbManager 打开设备，fd 经 libobs 的
// Android USB 总线（obs-android.h）传进来。fd 归 Java 所有，这里绝不 close(fd)，
// 只用 obs_android_usb_close() 归还。
//
// USB 事件/缓冲回收有独立接收线程；解析与 OBS 输出在源采集线程。
// 有界缓冲池通过所有权交接连接两者，停止时先取消并排干 USB，再释放句柄和图像缓冲。

#include <aaudio/AAudio.h>
#include <libusb-1.0/libusb.h>
#include <turbojpeg.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <obs-module.h>
#include "../android-common/android-locale.h"
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

/* obs.h 里的包含是 #ifdef __ANDROID__ 的，头本身与平台无关（同 nikon-source 的取舍） */
#include <obs-android.h>
#include "uvc-stream-buffer.h"
#include "uvc-bulk-pump.h"
#include "uvc-iso-select.h"
#include "uvc-iso-assembler.h"
#include "uvc-iso-pump.h"
#include "uvc-raw-payload.h"
#include "uvc-bulk-payload.h"
#include "uvc-backend-libuvc.h"
#include "uvc-cadence-stats.h"
#include "uvc-recovery.h"
#include "uvc-ts-smoother.h"

/* ---------------- UVC 常量（规范 4.3.1.1 / A.9） ---------------- */

#define UVC_CC_VIDEO 0x0E
#define UVC_SC_VIDEOCONTROL 0x01
#define UVC_SC_VIDEOSTREAMING 0x02

/* class-specific 描述符（CS_INTERFACE = 0x24）的 bDescriptorSubType */
#define VC_HEADER 0x01
#define VS_INPUT_HEADER 0x01
#define VS_FORMAT_UNCOMPRESSED 0x04
#define VS_FRAME_UNCOMPRESSED 0x05
#define VS_FORMAT_MJPEG 0x06
#define VS_FRAME_MJPEG 0x07

/* probe/commit 控制 */
#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
#define UVC_GET_LEN 0x85
#define UVC_GET_DEF 0x87
#define UVC_VS_PROBE_CONTROL 0x01
#define UVC_VS_COMMIT_CONTROL 0x02

/* UVC payload 头（表 2-5）：byte0 = HLE（头长），byte1 = BMH（位域） */
#define UVC_BMH_FID 0x01 /* Frame ID，帧间翻转 */
#define UVC_BMH_EOF 0x02 /* End of Frame */
#define UVC_BMH_ERR 0x40 /* 流错误：本 payload 起丢弃，下一 payload 开新帧 */
#define UVC_MAX_HLE 12

#define MAX_FRAME_BYTES (32U * 1024U * 1024U) /* 帧缓冲上限，防爆 */
#define MAX_MODES 128                         /* 单个 format 下的 frame 描述符总数上限 */

/* 连续 USB 错误超过这个数就放弃本次会话，走整体拆除 + 指数退避重开（同 nikon-source） */
#define MAX_CONSEC_USB_ERR 5
#define RETRY_BACKOFF_MIN_MS 1000ULL
#define RETRY_BACKOFF_MAX_MS 8000ULL
#define PERM_POLL_MS 500ULL
/* 同一实例连续这么多轮没送出任何帧 = 实例已死（IN 路径全 -1，只有拔插能救），
 * 退避拉到 30s 一档，别 1 秒一轮地锤（真机 1617：猝死后 GET probe -1 无限循环） */
#define DEAD_ROUND_LIMIT 5U
#define DEAD_ROUND_BACKOFF_MS 30000ULL

/* bulk 读超时：设备正常出流时读块间隔远小于它；超时只意味着断流/拔线 */
#define BULK_TIMEOUT_MS 1000
/* Fixed buffer pool: transfer completion hands ownership to the parser queue,
 * swaps a spare buffer into the transfer and resubmits without copying pixels. */
#define BULK_CHUNK_SIZE UVC_BULK_CHUNK_SIZE
#define XFER_IN_FLIGHT UVC_BULK_IN_FLIGHT
/* Valid-frame recovery deadlines and reset rate limiting are in uvc-recovery.h. */
/* 纯字节扫描状态机（不定长 payload）：误同步防护靠 PTS 一致性（帧内所有头的 PTS
 * 相等，随机数据撞上的概率约 2^-32）+ FID 一致性。边界只信 EOF（这台设备用空
 * payload 带 EOF 位收尾每帧）；连续拒这么多"边界声称"候选 = EOF 真丢了，重同步 */
#define REJECT_RESYNC 64U
/* 连续坏帧这么多 = 同步多半是假的，重同步 */
#define BADFRAME_RESYNC 30U
/* 流缓冲高水位：攒这么多还定不了下一个头 = 流异常，丢前半重扫（必须 < sbuf_cap - 读块） */
#define SBUF_HIGH_WATER (768U * 1024U)
/* MJPEG 送帧校验：FFD9 允许距帧尾这么多字节内的填充 */
#define FRAME_TAIL_SEARCH 512U
/* 连续失步这么多次后 hex dump 当前位置 64 字节诊断（每轮只打这一次） */
#define RESYNC_DUMP_AFTER 3


/* 未压缩 format GUID 的前 4 字节就是 FourCC（小端 ASCII，UVC 规范附录 A） */
#define FOURCC_YUY2 0x32595559 /* "YUY2" */
#define FOURCC_UYVY 0x59565955 /* "UYVY" */
#define FOURCC_NV12 0x3231564E /* "NV12" */
#define FOURCC_P010 0x30313050 /* "P010" */
#define FOURCC_RGB3 0x33424752 /* "RGB3" */
#define FOURCC_BGR3 0x33524742 /* "BGR3" */

enum uvc_mode_kind {
	MODE_MJPEG = 0,
	MODE_YUYV,
	MODE_NV12,
	MODE_RGB24,
	MODE_BGR24,
	MODE_P010,
	MODE_UYVY,
};

enum uvc_backend_kind {
	UVC_BACKEND_NATIVE = 0,
	UVC_BACKEND_LIBUVC = 1,
};

enum {
	UVC_AUDIO_AUTO = -2,
	UVC_AUDIO_DISABLED = -1,
	UVC_AUDIO_SYSTEM_DEFAULT = 0,
};

static bool p010_available;

/* 一个可选模式 = format 描述符 × frame 描述符摊平后的结果 */
struct uvc_mode {
	enum uvc_mode_kind kind;
	uint8_t fmt_index;
	uint8_t frame_index;
	uint16_t w;
	uint16_t h;
	uint8_t bpp;
	uint32_t max_frame_size;
	uint32_t def_interval; /* 100 ns 单位 */
	/* bFrameIntervalType==0 时是连续区间 [min,max] 步长 step；否则是离散表 */
	uint32_t min_iv;
	uint32_t max_iv;
	uint32_t step_iv;
	uint32_t intervals[16];
	int n_intervals;
};

struct uvc_retire_gate {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	bool monotonic_cond;
	unsigned active_jobs;
	unsigned refs; /* 1 owned by uvc_source + 1 per live background retire thread */
};

enum uvc_wait_kind {
	UVC_WAIT_RECOVER = 0,    /* transient transport/stream failure -> backoff + dead_rounds */
	UVC_WAIT_PERMISSION = 1, /* waiting on Android USB permission prompt */
	UVC_WAIT_RETIRE = 2,     /* waiting on background USB retire drain */
	UVC_WAIT_DEVICE = 3,     /* device missing / USB bus not ready -> gentle wait, no dead_rounds */
	UVC_WAIT_CONFIG = 4,     /* unsupported mode/format/device or none selected -> park until config/device changes */
	UVC_WAIT_BANDWIDTH = 5,  /* transient USB ISO bandwidth contention -> retry periodically or on topology change */
};

struct uvc_source {
	obs_source_t *source;

	/* 设置快照（usb_device 是 UsbDevice.getDeviceName()，形如 /dev/bus/usb/002/002） */
	char usb_device[OBS_ANDROID_USB_NAME_LEN];
	int width;
	int height;
	int fps;
	int prefer_fmt; /* enum uvc_mode_kind，-1 = 自动 */
	bool yuv_low_latency;
	char mode_setting[48];
	uint32_t requested_interval;
	enum video_colorspace colorspace;
	atomic_int color_range;
	int32_t audio_device;
	struct uvc_mode device_modes[MAX_MODES];
	int device_mode_count;
	char mode_device[OBS_ANDROID_USB_NAME_LEN];
	enum uvc_backend_kind mode_backend;
	bool negotiated_valid;
	enum uvc_mode_kind negotiated_kind;
	uint16_t negotiated_w;
	uint16_t negotiated_h;
	uint32_t negotiated_interval;
	char negotiated_device[OBS_ANDROID_USB_NAME_LEN];
	struct obs_source_frame2 raw_frame;
	struct uvc_ts_smoother ts_smoother;
	uint64_t ts_last_frames_dropped;
	struct uvc_cadence_stats cadence;
	struct uvc_reset_limit reset_limit; /* persists across sessions for this source */
	struct uvc_retire_gate *retire_gate;
	bool bulk_raw_fallback;
	bool bulk_payload_fallback;
	enum uvc_wait_kind wait_kind;
	char blocked_device[OBS_ANDROID_USB_NAME_LEN];
	bool backend_switch_settle;
	enum uvc_backend_kind backend;
	char fmt_setting[16]; /* video_format 设置的原始字符串，打日志消歧义用 */
	char usb_id[16];      /* 首台匹配设备的 "vid:pid"（label 前缀），拔插换号后按它认设备 */

	/* 采集线程与拆除协议（照 nikon-source/camera2-input.c：shutting_down 置 1 后 join） */
	pthread_t thread;
	volatile long thread_started;
	volatile long shutting_down;

	/* USB identity, capture status and descriptor snapshot shared with the properties dialog. */
	pthread_mutex_t mtx;
	char status[256];

	/* 只写给人看的计数：采集线程写、属性页读，不做任何判定依据 */
	_Atomic uint64_t frames_out;
	_Atomic uint64_t frames_dropped;
	_Atomic uint32_t cur_w;
	_Atomic uint32_t cur_h;
	_Atomic uint32_t cur_fps_x100; /* 实测帧率 ×100（每秒估一次） */
};

/* 一轮采集会话的全部运行期状态，只活在采集线程栈上 */
struct uvc_dev {
	libusb_context *ctx;
	libusb_device_handle *h;
	int bridge_fd; /* 从总线借来的 fd，归还用 obs_android_usb_close */

	int vc_iface;
	int vs_iface;
	int vs_alt; /* VS 的工作 altsetting（Bulk 通常为 0；ISO 协商后设为目标带宽档） */
	uint8_t ep_in;
	bool is_iso;
	bool bulk_raw_fallback;
	bool bulk_payload_fallback;
	struct uvc_iso_alt iso_alts[UVC_ISO_MAX_ALTS];
	int n_iso_alts;
	uint32_t iso_packet_size;
	uint8_t iso_interval;
	uint16_t uvc_version;  /* bcdUVC，决定 probe 结构长度 */
	uint32_t probe_len;    /* 26（1.0）/ 34（1.1）/ 48（1.5），GET_LEN 优先 */
	uint32_t clock_frequency; /* raw PROBE dwClockFrequency, diagnostic only */
	uint32_t payload_size; /* dwMaxPayloadTransferSize（0 = 设备没报，读块用固定大块） */
	int usb_speed;

	struct uvc_mode modes[MAX_MODES];
	int n_modes;
	int chosen; /* modes[] 下标，-1 = 未选 */

	uint32_t quirks;           /* 命中 uvc_quirks 表的标志位（0 = 无） */
	int eff_fmt;               /* 生效的偏好格式（怪癖默认覆盖后；否则 = s->prefer_fmt） */
	int eff_w, eff_h, eff_fps; /* 生效的分辨率/帧率（怪癖默认覆盖后） */

	uint32_t frame_interval;      /* 协商结果，100 ns */
	uint32_t iso_override_interval; /* ISO 带宽降档覆盖帧间隔，0 = 按配置选择 */
	uint32_t frame_buf_size;      /* dwMaxVideoFrameSize 协商读回值（可能是脏值，只当下限参考） */
};

/* bulk 字节流的解析状态：设备会把几百个 payload 混进一次传输，必须跨读块攒流再切。
 * 这台设备 payload 数据长度不固定（存在 0 字节的 EOF 收尾 payload），一切定长/等距
 * 假设都不成立——纯字节扫描状态机：扫下一个合法头，两个头之间的字节归前一个头。 */
struct uvc_stream {
	struct uvc_stream_buffer input;
	size_t scan_off; /* 扫描游标：之前的字节已确认不是候选头起点 */

	uint8_t hle; /* 会话头长（同步时从首个合法头学来，0 = 未同步；这台 = 12） */

	uint64_t sync_read;     /* 未同步期间累计读入字节（诊断用） */
	unsigned resync_streak; /* 连续重同步次数，够 RESYNC_DUMP_AFTER 打一次 hex dump */
	unsigned rejected;      /* 连续被拒的"边界声称"候选头，够 REJECT_RESYNC 重同步 */
	unsigned bad_streak;    /* 连续坏帧数，够 BADFRAME_RESYNC 重同步 */
	bool diag_dumped;       /* 本轮已打过一次诊断 dump，之后不再打 */
	uint64_t frames_done;   /* 重组完成的帧数（含坏帧），打点限频用 */

	uint8_t *frame_buf; /* 当前帧的纯载荷重组（只含数据、绝不含 payload 头） */
	uint8_t *rgba_buf; /* Used for RGB-ordered pixels only. */
	size_t frame_cap;
	size_t frame_len;
	uint32_t frame_total; /* 未压缩帧的定长；MJPEG = 0（不定长） */
	bool assembling;
	uint8_t last_fid;
	uint32_t frame_pts; /* 当前帧登记的 PTS（帧内所有头的 PTS 应相等） */
	bool has_pts;

	uint64_t err_first_ns; /* 当前 BMH ERR 连续段起点 ns（0 = 不在段内） */
	uint64_t err_count;    /* 本会话累计 BMH ERR 头数 */

	uint64_t ll_err; /* 以下是限频日志槽：上次放行时间 ns（0 = 还没打过） */
	uint64_t ll_highwater;
	uint64_t ll_badframe;
	uint64_t ll_overflow;
	uint64_t ll_resync;
};

/* ---------------- 小端读写（UVC 描述符/控制结构全部 LSB first） ---------------- */

static uint16_t get_u16le(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static const char *mode_kind_name(enum uvc_mode_kind k)
{
	switch (k) {
	case MODE_MJPEG:
		return "MJPEG";
	case MODE_YUYV:
		return "YUYV";
	case MODE_NV12:
		return "NV12";
	case MODE_P010:
		return "P010";
	case MODE_UYVY:
		return "UYVY";
	case MODE_RGB24:
		return "RGB24";
	case MODE_BGR24:
		return "BGR24";
	default:
		return "?";
	}
}

static bool is_rgb24(int kind)
{
	return kind == MODE_RGB24 || kind == MODE_BGR24;
}

static uint32_t rgb24_stride(const struct uvc_mode *mode)
{
	const uint32_t packed = (uint32_t)mode->w * 3;
	const uint32_t aligned = (packed + 3) & ~3U;
	return mode->max_frame_size == (uint64_t)aligned * mode->h ? aligned : packed;
}

/* Saturate oversized descriptors so the allocated frame size cannot wrap. */
static uint32_t raw_frame_bytes(const struct uvc_mode *m)
{
	uint64_t bytes;

	if (m->kind == MODE_YUYV || m->kind == MODE_UYVY)
		bytes = (uint64_t)m->w * (uint64_t)m->h * 2;
	else if (m->kind == MODE_NV12)
		bytes = (uint64_t)m->w * (uint64_t)m->h * 3 / 2;
	else if (m->kind == MODE_P010)
		bytes = (uint64_t)m->w * m->h * 3;
	else if (is_rgb24(m->kind))
		bytes = (uint64_t)rgb24_stride(m) * m->h;
	else
		return 0;

	return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

static void set_status_key(struct uvc_source *s, const char *key, const char *const *args, size_t nargs)
{
	struct dstr msg;
	obs_android_format_locale(&msg, obs_module_text(key), args, nargs);
	pthread_mutex_lock(&s->mtx);
	obs_android_copy_utf8(s->status, sizeof(s->status), msg.array ? msg.array : "");
	pthread_mutex_unlock(&s->mtx);
	dstr_free(&msg);
}

static void set_status_int(struct uvc_source *s, const char *key, int value)
{
	char number[16];
	snprintf(number, sizeof(number), "%d", value);
	const char *args[] = {number};
	set_status_key(s, key, args, 1);
}

static void set_capture_status(struct uvc_source *s, const char *key, const char *format, unsigned width,
			       unsigned height, double fps)
{
	char w[16], h[16], rate[24];
	snprintf(w, sizeof(w), "%u", width);
	snprintf(h, sizeof(h), "%u", height);
	snprintf(rate, sizeof(rate), "%.2f", fps);
	const char *args[] = {format, w, h, rate};
	set_status_key(s, key, args, 4);
}

static inline bool uvc_stopping(struct uvc_source *s)
{
	return s && os_atomic_load_long(&s->shutting_down) != 0;
}

/* 可中断的睡眠：退避期间也要对 deactivate/destroy 及时响应 */
static void sleep_check(struct uvc_source *s, uint32_t ms)
{
	while (ms-- && !uvc_stopping(s))
		os_sleep_ms(1);
}

static void uvc_format_mode_label(struct dstr *out, enum uvc_mode_kind kind, uint16_t w, uint16_t h,
				  uint32_t interval)
{
	char width[8], height[8], fps[16];
	snprintf(width, sizeof(width), "%u", (unsigned)w);
	snprintf(height, sizeof(height), "%u", (unsigned)h);
	snprintf(fps, sizeof(fps), "%.4g", interval ? 10000000.0 / interval : 0.0);
	const char *args[] = {mode_kind_name(kind), width, height, fps};
	obs_android_format_locale(out, obs_module_text("DeviceMode.Label"), args, 4);
}

static bool uvc_modes_equal(const struct uvc_mode *a, int na, const struct uvc_mode *b, int nb)
{
	if (na != nb)
		return false;
	for (int i = 0; i < na; i++) {
		if (a[i].kind != b[i].kind || a[i].fmt_index != b[i].fmt_index ||
		    a[i].frame_index != b[i].frame_index || a[i].w != b[i].w || a[i].h != b[i].h ||
		    a[i].bpp != b[i].bpp || a[i].max_frame_size != b[i].max_frame_size ||
		    a[i].def_interval != b[i].def_interval || a[i].min_iv != b[i].min_iv ||
		    a[i].max_iv != b[i].max_iv || a[i].step_iv != b[i].step_iv ||
		    a[i].n_intervals != b[i].n_intervals)
			return false;
		for (int j = 0; j < a[i].n_intervals; j++) {
			if (a[i].intervals[j] != b[i].intervals[j])
				return false;
		}
	}
	return true;
}

static bool uvc_set_negotiated_mode(struct uvc_source *s, const char *dev_name, const struct uvc_mode *mode,
				    uint32_t interval)
{
	if (!s || !mode)
		return false;
	const char *dev = dev_name ? dev_name : "";
	pthread_mutex_lock(&s->mtx);
	const bool changed = !s->negotiated_valid || s->negotiated_kind != mode->kind ||
			     s->negotiated_w != mode->w || s->negotiated_h != mode->h ||
			     s->negotiated_interval != interval ||
			     strcmp(s->negotiated_device, dev) != 0;
	if (changed) {
		s->negotiated_valid = true;
		s->negotiated_kind = mode->kind;
		s->negotiated_w = mode->w;
		s->negotiated_h = mode->h;
		s->negotiated_interval = interval;
		snprintf(s->negotiated_device, sizeof(s->negotiated_device), "%s", dev);
	}
	pthread_mutex_unlock(&s->mtx);
	/* Always refresh the timestamp smoother period and reset phase on every
	 * negotiation (initial start, watchdog renegotiate, YUYV fallback). */
	uvc_ts_smoother_configure(&s->ts_smoother, interval);
	s->ts_last_frames_dropped = atomic_load_explicit(&s->frames_dropped, memory_order_relaxed);
	return changed;
}

static void uvc_clear_negotiated_mode(struct uvc_source *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->mtx);
	s->negotiated_valid = false;
	s->negotiated_kind = 0;
	s->negotiated_w = 0;
	s->negotiated_h = 0;
	s->negotiated_interval = 0;
	s->negotiated_device[0] = '\0';
	pthread_mutex_unlock(&s->mtx);
	uvc_ts_smoother_reset(&s->ts_smoother);
}

static void uvc_notify_properties_updated(struct uvc_source *s)
{
	if (s && s->source && !uvc_stopping(s))
		obs_source_update_properties(s->source);
}

/* ---------------- VS 描述符解析（format/frame 摊平成 modes[]） ---------------- */

/* 帧间隔：fps → 100 ns 单位；离散表取最接近的一档，连续区间按步长取整夹取 */
static uint32_t pick_interval(const struct uvc_mode *m, int fps)
{
	if (fps <= 0)
		return m->def_interval;

	const uint32_t want = 10000000U / (uint32_t)fps;

	if (m->n_intervals > 0) {
		uint32_t best = m->intervals[0];
		for (int i = 1; i < m->n_intervals; i++) {
			const uint32_t iv = m->intervals[i];
			const int64_t d_old = (int64_t)best - (int64_t)want;
			const int64_t d_new = (int64_t)iv - (int64_t)want;
			if (d_new * d_new < d_old * d_old)
				best = iv;
		}
		return best;
	}
	if (m->min_iv == 0 || m->max_iv == 0)
		return m->def_interval;

	uint32_t iv = want < m->min_iv ? m->min_iv : want > m->max_iv ? m->max_iv : want;
	if (m->step_iv)
		iv = m->min_iv + ((iv - m->min_iv) / m->step_iv) * m->step_iv;
	return iv;
}

/* 往 modes[] 里追加一个 frame；返回 false = 满了 */
static bool add_mode(struct uvc_dev *d, enum uvc_mode_kind kind, uint8_t fmt_index, uint8_t bpp,
		     const uint8_t *f /* FRAME 描述符本体（含头 3 字节） */, size_t flen)
{
	if (d->n_modes >= MAX_MODES || flen < 26)
		return false;

	struct uvc_mode *m = &d->modes[d->n_modes];
	memset(m, 0, sizeof(*m));
	m->kind = kind;
	m->fmt_index = fmt_index;
	m->frame_index = f[3];
	m->bpp = bpp;
	m->w = get_u16le(f + 5);
	m->h = get_u16le(f + 7);
	m->max_frame_size = get_u32le(f + 17);
	m->def_interval = get_u32le(f + 21);
	m->min_iv = m->def_interval;
	m->max_iv = m->def_interval;
	m->step_iv = 0;

	const uint8_t itype = f[25];
	if (itype == 0) {
		/* 连续区间：dwMinFrameInterval/dwMaxFrameInterval/dwFrameIntervalStep */
		if (flen >= 38) {
			m->min_iv = get_u32le(f + 26);
			m->max_iv = get_u32le(f + 30);
			m->step_iv = get_u32le(f + 34);
		}
	} else {
		const int n = itype > 16 ? 16 : itype;
		for (int i = 0; i < n && 26 + 4 * (i + 1) <= (int)flen; i++)
			m->intervals[m->n_intervals++] = get_u32le(f + 26 + 4 * i);
	}

	if (m->w == 0 || m->h == 0)
		return true; /* 非法 frame 不致命，跳过但不记 */
	if (((kind == MODE_YUYV || kind == MODE_UYVY) && (m->w & 1)) ||
	    ((kind == MODE_NV12 || kind == MODE_P010) && ((m->w | m->h) & 1)))
		return true;
	d->n_modes++;
	return true;
}

static int uncompressed_kind(const uint8_t *guid, uint8_t bpp)
{
	const uint32_t fourcc = get_u32le(guid);
	if (fourcc == FOURCC_YUY2)
		return MODE_YUYV;
	if (fourcc == FOURCC_UYVY)
		return MODE_UYVY;
	if (fourcc == FOURCC_NV12)
		return MODE_NV12;
	static const uint8_t fourcc_suffix[] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
					       0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
	if (fourcc == FOURCC_P010 && p010_available && memcmp(guid + 4, fourcc_suffix, sizeof(fourcc_suffix)) == 0)
		return MODE_P010;
	if (bpp != 24)
		return -1;
	if (memcmp(guid + 4, fourcc_suffix, sizeof(fourcc_suffix)) == 0) {
		if (fourcc == FOURCC_RGB3)
			return MODE_RGB24;
		if (fourcc == FOURCC_BGR3)
			return MODE_BGR24;
	}

	/* The e436eb7d RGB24 subtype stores B, G, R. Use the wire order used by libuvc. */
	static const uint8_t rgb_subtype_suffix[] = {0x4f, 0x52, 0xce, 0x11, 0x9f, 0x53,
						  0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70};
	if (memcmp(guid + 4, rgb_subtype_suffix, sizeof(rgb_subtype_suffix)) == 0) {
		if (fourcc == 0xe436eb7d)
			return MODE_BGR24;
		if (fourcc == 0xe436eb7e)
			return MODE_RGB24;
	}
	return -1;
}

/* Collect frames for each recognised format from this alternate setting. */
static void parse_vs_extra(struct uvc_dev *d, const uint8_t *extra, int len)
{
	enum uvc_mode_kind cur_kind = MODE_MJPEG;
	uint8_t cur_fmt = 0;
	uint8_t cur_bpp = 0;
	bool cur_valid = false;
	int off = 0;

	while (off + 3 <= len) {
		const uint8_t dlen = extra[off];
		const uint8_t dtype = extra[off + 1];
		const uint8_t subtype = extra[off + 2];
		if (dlen < 3 || off + dlen > len)
			break;

		if (dtype == 0x24 /* CS_INTERFACE */) {
			/* 下面每个 case 读到的最大偏移都必须落在 dlen 之内：descriptor 是设备
			 * 上报的不可信数据，dlen 只保证 >= 3。原先只看 dlen >= 3 就按固定偏移读，
			 * 短 descriptor（尤其链尾那一条）会越界读进下一条描述符甚至 extra 缓冲之外。 */
			switch (subtype) {
			case VS_FORMAT_MJPEG:
				cur_valid = false;
				if (dlen < 4) /* bmPreferredFrameIndex 在 off+3 */
					break;
				cur_kind = MODE_MJPEG;
				cur_fmt = extra[off + 3];
				cur_bpp = 0;
				cur_valid = true;
				break;
			case VS_FORMAT_UNCOMPRESSED: {
				cur_valid = false;
				if (dlen < 22)
					break;
				cur_fmt = extra[off + 3];
				cur_bpp = extra[off + 21];
				const int kind = uncompressed_kind(extra + off + 5, cur_bpp);
				cur_valid = kind >= 0;
				if (cur_valid)
					cur_kind = (enum uvc_mode_kind)kind;
				break;
			}
			case VS_FRAME_MJPEG:
			case VS_FRAME_UNCOMPRESSED:
				if (cur_valid && ((subtype == VS_FRAME_MJPEG) == (cur_kind == MODE_MJPEG)))
					add_mode(d, cur_kind, cur_fmt, cur_bpp, extra + off, dlen);
				break;
			default:
				break;
			}
		}
		off += dlen;
	}
}

/* 从 VS extra 链里提取 VS_INPUT_HEADER.bEndpointAddress（若无或不合法则返回 0） */
static uint8_t parse_vs_input_header_ep(const uint8_t *extra, int len)
{
	int off = 0;
	while (off + 3 <= len) {
		const uint8_t dlen = extra[off];
		const uint8_t dtype = extra[off + 1];
		const uint8_t subtype = extra[off + 2];
		if (dlen < 3 || off + dlen > len)
			break;
		if (dtype == 0x24 && subtype == VS_INPUT_HEADER && dlen >= 7) {
			const uint8_t ep = extra[off + 6];
			if ((ep & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
				return ep;
		}
		off += dlen;
	}
	return 0;
}

/* 读配置描述符，找出 VC/VS 接口、Bulk 或 ISO IN 端点、bcdUVC，并解析全部 VS 模式。 */
static bool uvc_scan_device(struct uvc_dev *d)
{
	struct libusb_config_descriptor *cfg = NULL;
	libusb_device *dev = libusb_get_device(d->h);

	d->vc_iface = -1;
	d->vs_iface = -1;
	d->vs_alt = 0;
	d->ep_in = 0;
	d->uvc_version = 0x0100;
	d->n_modes = 0;
	d->is_iso = false;
	d->n_iso_alts = 0;
	d->iso_packet_size = 0;
	d->iso_interval = 0;
	d->iso_override_interval = 0;

	int r = libusb_get_active_config_descriptor(dev, &cfg);
	if (r != 0)
		r = libusb_get_config_descriptor(dev, 0, &cfg);
	if (r != 0 || !cfg) {
		blog(LOG_ERROR, "uvc: 读不到配置描述符（%d）", r);
		return false;
	}

	/* Pass 1: locate VideoControl interface and bcdUVC. */
	for (int i = 0; i < cfg->bNumInterfaces && d->vc_iface < 0; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];
		for (int a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
			if (alt->bInterfaceClass != UVC_CC_VIDEO || alt->bInterfaceSubClass != UVC_SC_VIDEOCONTROL)
				continue;
			d->vc_iface = alt->bInterfaceNumber;
			const uint8_t *ex = alt->extra;
			int off = 0;
			while (off + 3 <= alt->extra_length) {
				const uint8_t dlen = ex[off];
				if (dlen < 3 || off + dlen > alt->extra_length)
					break;
				if (ex[off + 1] == 0x24 && ex[off + 2] == VC_HEADER && dlen >= 5) {
					d->uvc_version = get_u16le(ex + off + 3);
					break;
				}
				off += dlen;
			}
			break;
		}
	}

	/* Pass 2: evaluate all VideoStreaming interfaces. Always prefer a usable
	 * Bulk VS interface first (preserving baseline behavior on multi-VS
	 * devices); fall back to the first usable Isochronous VS interface. */
	bool found_bulk = false;
	int first_iso_cfg_idx = -1;
	int first_iso_iface = -1;
	struct uvc_iso_alt first_iso_alts[UVC_ISO_MAX_ALTS];
	int first_iso_n_alts = 0;

	for (int i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];
		int iface_num = -1;
		uint8_t header_ep = 0;
		uint8_t bulk_ep_match = 0;
		int bulk_alt_match = 0;
		uint8_t bulk_ep_any = 0;
		int bulk_alt_any = 0;
		struct uvc_iso_alt cand_iso[UVC_ISO_MAX_ALTS];
		int cand_n_iso = 0;

		for (int a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
			if (alt->bInterfaceClass != UVC_CC_VIDEO || alt->bInterfaceSubClass != UVC_SC_VIDEOSTREAMING)
				continue;
			if (iface_num < 0)
				iface_num = alt->bInterfaceNumber;
			if (!header_ep && alt->extra_length > 0)
				header_ep = parse_vs_input_header_ep(alt->extra, alt->extra_length);

			for (int e = 0; e < alt->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
				if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN)
					continue;
				const uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
				if (type == LIBUSB_TRANSFER_TYPE_BULK) {
					if (!bulk_ep_any) {
						bulk_ep_any = ep->bEndpointAddress;
						bulk_alt_any = alt->bAlternateSetting;
					}
					if (header_ep && ep->bEndpointAddress == header_ep && !bulk_ep_match) {
						bulk_ep_match = ep->bEndpointAddress;
						bulk_alt_match = alt->bAlternateSetting;
					}
				} else if (type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
					struct libusb_ss_endpoint_companion_descriptor *ss_comp = NULL;
					const int ss_r = libusb_get_ss_endpoint_companion_descriptor(d->ctx, ep,
												     &ss_comp);
					const bool has_ss = ss_r == 0 && ss_comp != NULL;
					const uint32_t pkt_size = uvc_iso_calc_packet_size(
						ep->wMaxPacketSize, has_ss, has_ss ? ss_comp->wBytesPerInterval : 0,
						has_ss ? ss_comp->bMaxBurst : 0, has_ss ? ss_comp->bmAttributes : 0);
					if (ss_comp)
						libusb_free_ss_endpoint_companion_descriptor(ss_comp);
					if (pkt_size > 0 && cand_n_iso < UVC_ISO_MAX_ALTS) {
						cand_iso[cand_n_iso++] = (struct uvc_iso_alt){
							.alt_setting = alt->bAlternateSetting,
							.ep_in = ep->bEndpointAddress,
							.b_interval = ep->bInterval,
							.max_packet_size = pkt_size,
						};
					}
				}
			}
		}

		if (iface_num < 0)
			continue;

		int iso_match_count = 0;
		if (header_ep) {
			for (int k = 0; k < cand_n_iso; k++) {
				if (cand_iso[k].ep_in == header_ep)
					iso_match_count++;
			}
		}

		bool choose_bulk = false;
		bool choose_iso = false;
		if (header_ep) {
			if (bulk_ep_match)
				choose_bulk = true;
			else if (iso_match_count > 0)
				choose_iso = true;
			else if (bulk_ep_any && cand_n_iso == 0)
				choose_bulk = true;
			else if (cand_n_iso > 0)
				choose_iso = true;
		} else {
			if (bulk_ep_any && cand_n_iso == 0)
				choose_bulk = true;
			else if (cand_n_iso > 0)
				choose_iso = true;
		}

		if (!choose_bulk && !choose_iso)
			continue;

		d->n_modes = 0;
		for (int a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
			if (alt->bInterfaceClass == UVC_CC_VIDEO && alt->bInterfaceSubClass == UVC_SC_VIDEOSTREAMING &&
			    alt->extra_length > 0)
				parse_vs_extra(d, alt->extra, alt->extra_length);
		}
		if (d->n_modes == 0) {
			blog(LOG_INFO, "uvc: VS 接口 %d 无可用视频模式，继续扫描后续接口", iface_num);
			continue;
		}

		if (choose_bulk) {
			d->is_iso = false;
			d->vs_iface = iface_num;
			d->vs_alt = bulk_ep_match ? bulk_alt_match : bulk_alt_any;
			d->ep_in = bulk_ep_match ? bulk_ep_match : bulk_ep_any;
			found_bulk = true;
			break;
		}

		if (first_iso_cfg_idx < 0) {
			if (header_ep && iso_match_count > 0 && iso_match_count < cand_n_iso) {
				int out_k = 0;
				for (int k = 0; k < cand_n_iso; k++) {
					if (cand_iso[k].ep_in == header_ep)
						cand_iso[out_k++] = cand_iso[k];
				}
				cand_n_iso = out_k;
			}
			uvc_iso_sort_alts(cand_iso, cand_n_iso);
			memcpy(first_iso_alts, cand_iso, (size_t)cand_n_iso * sizeof(cand_iso[0]));
			first_iso_n_alts = cand_n_iso;
			first_iso_cfg_idx = i;
			first_iso_iface = iface_num;
		}
	}

	if (!found_bulk) {
		d->n_modes = 0;
		if (first_iso_cfg_idx >= 0) {
			const struct libusb_interface *itf = &cfg->interface[first_iso_cfg_idx];
			for (int a = 0; a < itf->num_altsetting; a++) {
				const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
				if (alt->bInterfaceClass == UVC_CC_VIDEO &&
				    alt->bInterfaceSubClass == UVC_SC_VIDEOSTREAMING && alt->extra_length > 0)
					parse_vs_extra(d, alt->extra, alt->extra_length);
			}
			memcpy(d->iso_alts, first_iso_alts, (size_t)first_iso_n_alts * sizeof(first_iso_alts[0]));
			d->n_iso_alts = first_iso_n_alts;
			d->is_iso = true;
			d->vs_iface = first_iso_iface;
			d->vs_alt = 0;
			d->ep_in = d->iso_alts[0].ep_in;
		}
	}
	libusb_free_config_descriptor(cfg);

	if (d->is_iso && d->n_iso_alts > 0) {
		blog(LOG_INFO,
		     "uvc: 描述符扫描：VC 接口=%d VS 接口=%d iso-IN=0x%02X alt档数=%d（最大包长 %u..%u） bcdUVC=0x%04X 模式数=%d",
		     d->vc_iface, d->vs_iface, d->ep_in, d->n_iso_alts,
		     (unsigned)d->iso_alts[0].max_packet_size,
		     (unsigned)d->iso_alts[d->n_iso_alts - 1].max_packet_size, (unsigned)d->uvc_version, d->n_modes);
		for (int k = 0; k < d->n_iso_alts; k++) {
			blog(LOG_INFO, "uvc:   ISO alt=%d ep=0x%02X pkt=%u interval=%u", d->iso_alts[k].alt_setting,
			     d->iso_alts[k].ep_in, (unsigned)d->iso_alts[k].max_packet_size,
			     (unsigned)d->iso_alts[k].b_interval);
		}
	} else {
		blog(LOG_INFO, "uvc: 描述符扫描：VC 接口=%d VS 接口=%d bulk-IN=0x%02X alt=%d bcdUVC=0x%04X 模式数=%d",
		     d->vc_iface, d->vs_iface, d->ep_in, d->vs_alt, (unsigned)d->uvc_version, d->n_modes);
	}

	if (d->vs_iface < 0 || d->ep_in == 0 || d->n_modes == 0) {
		blog(LOG_ERROR, "uvc: 未找到可用的 UVC 流接口或视频模式（设备可能不符合标准 UVC 规范）");
		return false;
	}

	for (int i = 0; i < d->n_modes; i++) {
		const struct uvc_mode *m = &d->modes[i];
		blog(LOG_INFO, "uvc:   模式[%d] %s fmt=%u frame=%u %ux%u 帧间隔=%u(100ns) 帧率档数=%d 最大帧长=%u", i,
		     mode_kind_name(m->kind), (unsigned)m->fmt_index, (unsigned)m->frame_index, (unsigned)m->w,
		     (unsigned)m->h, (unsigned)m->def_interval, m->n_intervals, (unsigned)m->max_frame_size);
	}
	return true;
}

/* ---------------- 设备怪癖表（vid:pid → 行为标志，真机实测录入，勿凭猜添加） ---------------- */

#define QUIRK_PREFER_YUYV 0x01 /* MJPEG/NV12 通路出全帧黑场（固件 bug），仅 YUYV 出真实画面 */
#define QUIRK_ISO_IGNORE_PAYLOAD 0x02 /* Confirmed firmware misreports dwMaxPayloadTransferSize on ISO */

struct uvc_quirk {
	uint16_t vid;
	uint16_t pid;
	uint32_t flags;
};

static const struct uvc_quirk uvc_quirks[] = {
	/* UGREEN 95348（MS2130 类）：MJPEG/NV12 全帧黑场，Windows 上同样复现；
	 * YUYV 各分辨率只有 166666（60fps）一档离散帧率 */
	{0x2b89, 0x5348, QUIRK_PREFER_YUYV},
};

static uint32_t device_quirks(libusb_device_handle *handle)
{
	libusb_device *dev = handle ? libusb_get_device(handle) : NULL;
	struct libusb_device_descriptor descriptor;
	if (!dev || libusb_get_device_descriptor(dev, &descriptor) != 0)
		return 0;
	for (size_t i = 0; i < sizeof(uvc_quirks) / sizeof(uvc_quirks[0]); i++)
		if (uvc_quirks[i].vid == descriptor.idVendor && uvc_quirks[i].pid == descriptor.idProduct)
			return uvc_quirks[i].flags;
	return 0;
}

static bool mode_is_usable(uint32_t quirks, enum uvc_mode_kind kind)
{
	return !(quirks & QUIRK_PREFER_YUYV) || (kind != MODE_NV12 && kind != MODE_MJPEG);
}

static int copy_usable_modes(struct uvc_mode *out, const struct uvc_dev *device)
{
	int count = 0;
	for (int i = 0; i < device->n_modes; ++i) {
		if (mode_is_usable(device->quirks, device->modes[i].kind))
			out[count++] = device->modes[i];
	}
	return count;
}

/* ---------------- 模式选择（优先用户设置，回退 MJPEG 1080p → 最大 MJPEG → 最大未压缩） ---------------- */

static bool is_yuv_mode_kind(int kind)
{
	return kind == MODE_YUYV || kind == MODE_UYVY || kind == MODE_NV12 || kind == MODE_P010;
}

static bool raw_low_latency(const struct uvc_source *s)
{
	return s->yuv_low_latency && is_yuv_mode_kind(s->prefer_fmt);
}

/* Accept 59.94 for a requested 60, but never silently accept 30/15 fps. */
static bool interval_matches(uint32_t interval, int fps)
{
	if (!interval || fps <= 0)
		return false;
	const uint64_t actual = (uint64_t)interval * (uint32_t)fps;
	const uint64_t delta = actual > 10000000ULL ? actual - 10000000ULL : 10000000ULL - actual;
	return delta <= 50000ULL; /* 0.5% tolerance, including integer interval rounding. */
}

static int mode_bandwidth_rank(enum uvc_mode_kind kind)
{
	switch (kind) {
	case MODE_MJPEG:
		return 3;
	case MODE_NV12:
		return 2;
	case MODE_YUYV:
	case MODE_UYVY:
		return 1;
	default:
		return 0;
	}
}

static int mode_score(const struct uvc_dev *d, int idx, const struct uvc_source *s)
{
	const struct uvc_mode *m = &d->modes[idx];
	UNUSED_PARAMETER(s); /* 评分全部用怪癖覆盖后的生效值（d->eff_*） */

	/* 用户指定的格式 + 分辨率命中（用怪癖覆盖后的生效值） */
	if (d->eff_fmt >= 0 && m->kind == (enum uvc_mode_kind)d->eff_fmt && m->w == d->eff_w && m->h == d->eff_h)
		return 100;
	/* 用户指定的格式 + 1080p */
	if (d->eff_fmt >= 0 && m->kind == (enum uvc_mode_kind)d->eff_fmt && m->w == 1920 && m->h == 1080)
		return 90;
	/* MJPEG 1080p */
	if (m->kind == MODE_MJPEG && m->w == 1920 && m->h == 1080)
		return 80;
	/* 用户指定分辨率（格式没匹配上时） */
	if (m->w == d->eff_w && m->h == d->eff_h)
		return 70;
	/* 用户指定格式（设备不支持所选分辨率/1080p 时，仍优先在该格式下挑最大分辨率） */
	if (d->eff_fmt >= 0 && m->kind == (enum uvc_mode_kind)d->eff_fmt)
		return 60;
	return -1; /* 交给面积回退 */
}

static bool mode_has_interval(const struct uvc_mode *mode, uint32_t interval)
{
	for (int i = 0; i < mode->n_intervals; ++i) {
		if (mode->intervals[i] == interval)
			return true;
	}
	if (mode->n_intervals > 0)
		return false;
	if (mode->min_iv && mode->max_iv)
		return interval >= mode->min_iv && interval <= mode->max_iv &&
		       (!mode->step_iv || (interval - mode->min_iv) % mode->step_iv == 0);
	return interval == mode->def_interval;
}

static int choose_mode(struct uvc_dev *d, const struct uvc_source *s)
{
	if ((s->requested_interval || raw_low_latency(s)) &&
	    !mode_is_usable(d->quirks, (enum uvc_mode_kind)s->prefer_fmt)) {
		blog(LOG_WARNING, "uvc: 此设备已知 %s 输出黑场，请选择 YUYV",
		     mode_kind_name((enum uvc_mode_kind)s->prefer_fmt));
		return -1;
	}
	if (s->requested_interval) {
		for (int i = 0; i < d->n_modes; ++i) {
			const struct uvc_mode *mode = &d->modes[i];
			if (mode->kind == (enum uvc_mode_kind)s->prefer_fmt && mode->w == s->width && mode->h == s->height &&
			    mode_has_interval(mode, s->requested_interval))
				return i;
		}
		return -1;
	}
	if (raw_low_latency(s) || is_rgb24(s->prefer_fmt)) {
		int selected = -1;
		uint64_t best_delta = UINT64_MAX;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (m->kind != (enum uvc_mode_kind)s->prefer_fmt || m->w != s->width || m->h != s->height)
				continue;
			const uint32_t iv = pick_interval(m, s->fps);
			if (!interval_matches(iv, s->fps))
				continue;
			const uint64_t ticks = (uint64_t)iv * (uint32_t)s->fps;
			const uint64_t delta = ticks > 10000000ULL ? ticks - 10000000ULL : 10000000ULL - ticks;
			if (selected < 0 || delta < best_delta) {
				selected = i;
				best_delta = delta;
			}
		}
		return selected;
	}
	int best = -1;
	int best_score = -1;
	uint32_t best_area = 0;

	for (int i = 0; i < d->n_modes; i++) {
		const struct uvc_mode *m = &d->modes[i];
		if (!mode_is_usable(d->quirks, m->kind))
			continue;
		const int score = mode_score(d, i, s);
		if (score > best_score) {
			best_score = score;
			best = i;
			best_area = (uint32_t)m->w * (uint32_t)m->h;
			continue;
		}
		/* 同分或都没命中时：MJPEG 优先于未压缩，其次面积大者优先；
		 * 同分辨率未压缩档位在 USB 2.0 链路上优先带宽更低的格式（NV12 > YUYV/UYVY > RGB24）。 */
		if (score == best_score || (best_score < 0)) {
			const uint32_t area = (uint32_t)m->w * (uint32_t)m->h;
			const int kind_rank = m->kind == MODE_MJPEG ? 1 : 0;
			const int best_rank = best >= 0 && d->modes[best].kind == MODE_MJPEG ? 1 : 0;
			const bool usb2_bw_pref = d->usb_speed > 0 && d->usb_speed <= LIBUSB_SPEED_HIGH &&
						  kind_rank == best_rank && area == best_area && best >= 0 &&
						  mode_bandwidth_rank(m->kind) >
							  mode_bandwidth_rank(d->modes[best].kind);
			if (best < 0 || kind_rank > best_rank || (kind_rank == best_rank && area > best_area) ||
			    usb2_bw_pref) {
				best = i;
				best_area = area;
			}
		}
	}

	/* 怪癖改判：仅 YUYV 可用的卡，同分辨率有 YUYV 档就无条件改选（这台卡
	 * MJPEG/NV12 出全帧黑场，用户手选 MJPEG 也救不回来，日志说清原因） */
	if (best >= 0 && (d->quirks & QUIRK_PREFER_YUYV) && d->modes[best].kind != MODE_YUYV) {
		const struct uvc_mode *bm = &d->modes[best];
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (m->kind == MODE_YUYV && m->w == bm->w && m->h == bm->h) {
				blog(LOG_WARNING,
				     "uvc: 怪癖改选 YUYV（这台卡 MJPEG/NV12 出全帧黑场，仅 YUYV 可用；原选 %s %ux%u）",
				     mode_kind_name(bm->kind), (unsigned)bm->w, (unsigned)bm->h);
				best = i;
				break;
			}
		}
	}

	/* 真机教训改判：这类卡（MS2130）1080p 的未压缩档只有几 fps，能用的是 MJPEG。
	 * 评分若选出了非 MJPEG，但同分辨率有帧率更高的 MJPEG 档，改选 MJPEG 并把账打出来
	 * —— 显式选了未压缩的用户看到日志也知道为什么被改判、原档到底有多慢。
	 * （QUIRK_PREFER_YUYV 的卡跳过：那台 MJPEG 是黑场，绝不能往 MJPEG 改判） */
	if (best >= 0 && d->modes[best].kind != MODE_MJPEG && !(d->quirks & QUIRK_PREFER_YUYV)) {
		const struct uvc_mode *bm = &d->modes[best];
		const uint32_t biv = pick_interval(bm, d->eff_fps);
		const uint32_t best_fps100 = biv ? (uint32_t)(1000000000ULL / biv) : 0;
		int mj = -1;
		uint32_t mj_fps100 = 0;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (m->kind != MODE_MJPEG || m->w != bm->w || m->h != bm->h)
				continue;
			const uint32_t iv = pick_interval(m, d->eff_fps);
			const uint32_t f = iv ? (uint32_t)(1000000000ULL / iv) : 0;
			if (mj < 0 || f > mj_fps100) {
				mj = i;
				mj_fps100 = f;
			}
		}
		if (mj >= 0 && mj_fps100 > best_fps100) {
			blog(LOG_INFO,
			     "uvc: 改选 MJPEG（同为 %ux%u：%s 档最快 ≈%.2f fps，MJPEG 档 ≈%.2f fps；设置项 video_format='%s'）",
			     (unsigned)bm->w, (unsigned)bm->h, mode_kind_name(bm->kind), best_fps100 / 100.0,
			     mj_fps100 / 100.0, s->fmt_setting);
			best = mj;
		}
	}
	return best;
}

/* ---------------- probe/commit 协商（UVC 规范 4.3.1.1） ---------------- */

/* GET_LEN 问 probe 结构的实际长度；问不到就按 bcdUVC 推（1.0=26 / 1.1=34 / 1.5=48） */
static int probe_struct_len(struct uvc_dev *d)
{
	uint8_t l[2] = {0};
	const int r = libusb_control_transfer(d->h, 0xA1, UVC_GET_LEN, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface,
					      l, sizeof(l), 1000);
	if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_INVALID_PARAM)
		return r;
	if (r == 2) {
		const uint32_t n = get_u16le(l);
		if (n >= 26 && n <= 48)
			return n;
	}
	return d->uvc_version >= 0x0150 ? 48 : d->uvc_version >= 0x0110 ? 34 : 26;
}

/* 把协商结果从 probe 结构里读回 d（间隔 / 帧长 / payload 长） */
static void probe_readback(struct uvc_dev *d, const uint8_t *p, uint32_t len)
{
	d->clock_frequency = len >= 30 ? get_u32le(p + 26) : 0;
	if (len >= 26) {
		d->frame_interval = get_u32le(p + 4);
		d->frame_buf_size = get_u32le(p + 18);
		d->payload_size = get_u32le(p + 22);
	}
}

/* 一次协商尝试：probe 结构以 GET_DEF 设备默认打底（Windows UVC 驱动做法），只覆盖
 * bFormatIndex/bFrameIndex/dwFrameInterval 三项 → SET probe → GET probe（读回，
 * 失败重试 3 次间隔 200ms）→ SET probe（确认值）→ SET commit。
 * GET_DEF 拿不到回退历史手填版（黑场谜案：手填的 interval/maxframe/bmHint 可能把
 * 固件拐进黑场模式——设备按所请格式编出 CBR 黑帧，尺寸∝分辨率实锤）。
 * GET 3 次都挂就跳过 readback 直接 SET+commit 期望值（真机第四轮：拔插后这台卡
 * GET probe 永远 -1，但 SET 类 OUT 请求是收的；很多廉价设备 GET 本来就不稳）。
 * 返回 0 = 协商成功，d->frame_interval/frame_buf_size/payload_size 已填。 */
static int uvc_find_mode_by_index(const struct uvc_dev *d, uint8_t fmt_index, uint8_t frame_index)
{
	if (!d || fmt_index == 0 || frame_index == 0)
		return -1;
	for (int i = 0; i < d->n_modes; i++) {
		const struct uvc_mode *m = &d->modes[i];
		if (m->fmt_index == fmt_index && m->frame_index == frame_index &&
		    mode_is_usable(d->quirks, m->kind))
			return i;
	}
	return -1;
}

static int negotiate_try(struct uvc_dev *d, struct uvc_source *s, bool allow_mode_remap)
{
	uint8_t probe[48];
	uint8_t back[48];
	uint8_t hand[48];
	const struct uvc_mode *m = &d->modes[d->chosen];

	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;

	const int probe_len = probe_struct_len(d);
	if (probe_len < 0)
		return probe_len;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	d->probe_len = (uint32_t)probe_len;
	d->payload_size = 0;
	d->clock_frequency = 0;
	memset(probe, 0, sizeof(probe));
	memset(back, 0, sizeof(back));

	/* 帧间隔一次算清：离散表吸最近档，连续区间夹取；请求的 fps 不在表里时
	 * 明说吸到了哪档（真机误解实录：这卡 YUYV 只有 166666 一档，请求 30fps
	 * 实际跑 60fps = 双倍带宽，"稳定"其实是冻死的最后一帧） */
	const uint32_t want_iv =
		d->iso_override_interval ? d->iso_override_interval
					 : (s->requested_interval ? s->requested_interval : pick_interval(m, d->eff_fps));
	if (!d->iso_override_interval && !s->requested_interval && d->eff_fps > 0 && !interval_matches(want_iv, d->eff_fps))
		blog(LOG_WARNING, "uvc: 请求 %d fps 设备不支持（%s），吸到最近档 ≈%.2f fps（interval %u → %u）",
		     d->eff_fps, m->n_intervals > 0 ? "离散帧率表没有这一档" : "超出连续区间，已夹取",
		     10000000.0 / want_iv, 10000000U / (uint32_t)d->eff_fps, (unsigned)want_iv);

	/* 手填版（历史行为，留作对照打印 + GET_DEF 失败时的兜底）。bmHint bit0 =
	 * dwFrameInterval 要紧；dwMaxVideoFrameSize 按规范填期望值：这台卡读回过
	 * 16384 的脏值（真帧 4 MB），很多设备不在乎这个字段，但填对更稳 */
	memset(hand, 0, sizeof(hand));
	put_u16le(hand, 0x0001);
	hand[2] = m->fmt_index;
	hand[3] = m->frame_index;
	put_u32le(hand + 4, want_iv);
	put_u32le(hand + 18, m->kind == MODE_MJPEG ? m->max_frame_size : raw_frame_bytes(m));

	int r = libusb_control_transfer(d->h, 0xA1, UVC_GET_DEF, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface,
					probe, (uint16_t)d->probe_len, 1000);
	if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_INVALID_PARAM)
		return r;
	if (r == (int)d->probe_len) {
		char hex[48 * 3 + 1];
		for (uint32_t i = 0; i < d->probe_len; i++)
			snprintf(hex + i * 3, 4, "%02x ", probe[i]);
		blog(LOG_INFO, "uvc: GET_DEF probe（设备默认 %u 字节）：%s", (unsigned)d->probe_len, hex);
		blog(LOG_INFO,
		     "uvc: 设备默认 fmt=%u frame=%u interval=%u(≈%u fps) maxframe=%u payload=%u | "
		     "手填对照 interval=%u maxframe=%u",
		     (unsigned)probe[2], (unsigned)probe[3], (unsigned)get_u32le(probe + 4),
		     get_u32le(probe + 4) ? 10000000U / get_u32le(probe + 4) : 0U, (unsigned)get_u32le(probe + 18),
		     (unsigned)get_u32le(probe + 22), (unsigned)get_u32le(hand + 4), (unsigned)get_u32le(hand + 18));
		/* 只覆盖三项：bFormatIndex / bFrameIndex / dwFrameInterval（bmHint 等
		 * 其余字段保持设备默认，与 Windows 驱动一致） */
		probe[2] = m->fmt_index;
		probe[3] = m->frame_index;
		put_u32le(probe + 4, want_iv);
	} else {
		blog(LOG_WARNING, "uvc: GET_DEF probe 失败（r=%d），已回退使用构造默认值", r);
		memcpy(probe, hand, sizeof(probe));
	}

	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface, probe,
				    (uint16_t)d->probe_len, 1000);
	if (r != (int)d->probe_len) {
		blog(LOG_ERROR, "uvc: SET probe 失败 r=%d（想要 %u 字节）", r, (unsigned)d->probe_len);
		return r < 0 ? r : LIBUSB_ERROR_IO;
	}

	bool got = false;
	for (int t = 0; t < 3 && !got; t++) {
		if (t)
			sleep_check(s, 200);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		r = libusb_control_transfer(d->h, 0xA1, UVC_GET_CUR, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface,
					    back, (uint16_t)d->probe_len, 1000);
		if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_INVALID_PARAM ||
		    r == LIBUSB_ERROR_NOT_SUPPORTED)
			return r;
		got = r == (int)d->probe_len;
		if (!got)
			blog(LOG_WARNING, "uvc: GET probe 第 %d/3 次尝试失败（r=%d）", t + 1, r);
	}

	const bool strict_mode = raw_low_latency(s) || is_rgb24(m->kind) || s->requested_interval != 0;
	const bool check_interval = !d->iso_override_interval && strict_mode;
	const bool can_remap = allow_mode_remap && !strict_mode;
	int confirmed_idx = d->chosen;

	if (!got && check_interval) {
		blog(LOG_ERROR, "uvc: 无法确认设备接受了请求的视频模式");
		return r < 0 ? r : LIBUSB_ERROR_IO;
	}
	if (got) {
		const uint32_t reported_iv = get_u32le(back + 4);
		if (back[2] != m->fmt_index || back[3] != m->frame_index) {
			const int mapped = can_remap ? uvc_find_mode_by_index(d, back[2], back[3]) : -1;
			if (mapped < 0) {
				blog(LOG_WARNING,
				     "uvc: device declined mode %u:%u interval=%u (reported %u:%u/%u)",
				     (unsigned)m->fmt_index, (unsigned)m->frame_index, (unsigned)want_iv,
				     (unsigned)back[2], (unsigned)back[3], (unsigned)reported_iv);
				return LIBUSB_ERROR_NOT_SUPPORTED;
			}
			confirmed_idx = mapped;
		}
		if (check_interval) {
			bool interval_ok = mode_has_interval(m, reported_iv);
			if (s->requested_interval) {
				const uint32_t diff =
					reported_iv > want_iv ? reported_iv - want_iv : want_iv - reported_iv;
				interval_ok = diff <= 10U;
			} else if (raw_low_latency(s) || is_rgb24(s->prefer_fmt))
				interval_ok = interval_matches(reported_iv, s->fps);
			if (!interval_ok) {
				blog(LOG_WARNING,
				     "uvc: device declined mode %u:%u interval=%u (reported %u:%u/%u)",
				     (unsigned)m->fmt_index, (unsigned)m->frame_index, (unsigned)want_iv,
				     (unsigned)back[2], (unsigned)back[3], (unsigned)reported_iv);
				return LIBUSB_ERROR_NOT_SUPPORTED;
			}
		}
		probe_readback(d, back, d->probe_len);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		/* 用设备读回的值再 SET 一次 probe，然后 commit（规范流程，部分设备缺这步会拒） */
		r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface,
					    back, (uint16_t)d->probe_len, 1000);
		if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_INVALID_PARAM ||
		    r == LIBUSB_ERROR_NOT_SUPPORTED)
			return r;
		if (r != (int)d->probe_len)
			blog(LOG_WARNING, "uvc: 二次 SET probe 失败（r=%d），仍尝试提交 commit", r);
	} else {
		blog(LOG_WARNING, "uvc: GET probe 连续 3 次失败，跳过回读并直接尝试 SET+commit");
		memcpy(back, probe, sizeof(back));
		d->frame_interval = want_iv;
		d->frame_buf_size = m->kind == MODE_MJPEG ? m->max_frame_size : raw_frame_bytes(m);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8, (uint16_t)d->vs_iface,
					    back, (uint16_t)d->probe_len, 1000);
		if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_ACCESS || r == LIBUSB_ERROR_INVALID_PARAM ||
		    r == LIBUSB_ERROR_NOT_SUPPORTED)
			return r;
		if (r != (int)d->probe_len)
			blog(LOG_WARNING, "uvc: 回退路径二次 SET probe 失败（r=%d），仍尝试提交 commit", r);
	}

	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	r = libusb_control_transfer(d->h, 0x21, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL << 8, (uint16_t)d->vs_iface, back,
				    (uint16_t)d->probe_len, 1000);
	if (r != (int)d->probe_len) {
		blog(LOG_ERROR, "uvc: SET commit 失败（r=%d）", r);
		return r < 0 ? r : LIBUSB_ERROR_IO;
	}

	if (confirmed_idx != d->chosen) {
		const struct uvc_mode *cm = &d->modes[confirmed_idx];
		blog(LOG_WARNING,
		     "uvc: 设备将协商模式由 %s %ux%u (%u:%u) 调整为 %s %ux%u (%u:%u)，按确认模式重配",
		     mode_kind_name(m->kind), (unsigned)m->w, (unsigned)m->h, (unsigned)m->fmt_index,
		     (unsigned)m->frame_index, mode_kind_name(cm->kind), (unsigned)cm->w, (unsigned)cm->h,
		     (unsigned)cm->fmt_index, (unsigned)cm->frame_index);
		d->chosen = confirmed_idx;
		m = cm;
	}

	if (d->frame_interval == 0)
		d->frame_interval = m->def_interval;

	blog(LOG_INFO,
	     "uvc: 协商成功 —— %s %ux%u interval=%u(≈%u fps) maxframe=%u payload=%u（probe 结构 %u 字节，bcdUVC=0x%04X，readback %s）",
	     mode_kind_name(m->kind), (unsigned)m->w, (unsigned)m->h, (unsigned)d->frame_interval,
	     d->frame_interval ? 10000000U / d->frame_interval : 0U, (unsigned)d->frame_buf_size,
	     (unsigned)d->payload_size, (unsigned)d->probe_len, (unsigned)d->uvc_version, got ? "成功" : "跳过");
	blog(LOG_INFO, "uvc: event=probe_clock readback=%d clock_frequency=%u", got, (unsigned)d->clock_frequency);
	return 0;
}

/* 协商入口：先试一轮，失败就 SET_INTERFACE alt0→工作alt 踢一脚 VS 接口状态机再试一轮
 * （ISO 设备保持 alt0 重试，绝不在 COMMIT 前切入非零 ISO 带宽档）。每步打日志。 */
static int uvc_negotiate(struct uvc_dev *d, struct uvc_source *s, bool allow_mode_remap)
{
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	int r = negotiate_try(d, s, allow_mode_remap);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (d->vs_iface < 0 || (r != LIBUSB_ERROR_IO && r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_PIPE))
		return r;

	if (d->is_iso) {
		blog(LOG_WARNING, "uvc: 协商失败（r=%d），ISO 设备重置到 alt0 后重试", r);
		const int s0 = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (s0 != 0)
			return s0;
		sleep_check(s, 100);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		return negotiate_try(d, s, allow_mode_remap);
	}

	blog(LOG_WARNING, "uvc: 协商失败（r=%d），尝试切换接口备用设置 alt0 -> alt%d 后重试", r, d->vs_alt);
	const int s0 = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (s0 != 0)
		return s0;
	sleep_check(s, 100);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	const int s1 = libusb_set_interface_alt_setting(d->h, d->vs_iface, d->vs_alt);
	blog(LOG_WARNING, "uvc: SET_INTERFACE alt0 r=%d，工作 alt%d r=%d", s0, d->vs_alt, s1);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (s1 != 0)
		return s1;
	return negotiate_try(d, s, allow_mode_remap);
}

struct uvc_active_pump;
static bool start_active_pump(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump, int *error);
static int stop_active_pump(struct uvc_active_pump *pump);
static bool active_pump_cancel_timed_out(const struct uvc_active_pump *pump);
static unsigned active_pump_iso_pkts_per_xfer(const struct uvc_active_pump *pump);

/* Switches VS interface to alt, starts the ISO pump (submitting initial URBs),
 * and reverts to alt0 on initial URB submission failure (unless cancel timed out). */
static int uvc_try_activate_iso_alt(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump,
				    const struct uvc_iso_alt *alt, bool *failed_at_submit)
{
	if (failed_at_submit)
		*failed_at_submit = false;
	if (!pump)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (active_pump_cancel_timed_out(pump))
		return UVC_PUMP_ERR_CANCEL_TIMEOUT;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;

	const int r = libusb_set_interface_alt_setting(d->h, d->vs_iface, alt->alt_setting);
	if (uvc_stopping(s)) {
		if (r == 0)
			libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
		return LIBUSB_ERROR_INTERRUPTED;
	}
	if (r != 0)
		return r;

	d->vs_alt = alt->alt_setting;
	d->ep_in = alt->ep_in;
	d->iso_packet_size = alt->max_packet_size;
	d->iso_interval = alt->b_interval;

	int pump_err = 0;
	if (start_active_pump(d, s, pump, &pump_err)) {
		if (uvc_stopping(s)) {
			const int stop_res = stop_active_pump(pump);
			d->vs_alt = 0;
			d->iso_packet_size = 0;
			d->iso_interval = 0;
			if (stop_res == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
				return UVC_PUMP_ERR_CANCEL_TIMEOUT;
			libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
			return LIBUSB_ERROR_INTERRUPTED;
		}
		blog(LOG_INFO,
		     "uvc: 激活 ISO 接口 alt%d（ep=0x%02X pkt=%u×%u interval=%u，设备协商 payload=%u）",
		     d->vs_alt, d->ep_in, (unsigned)d->iso_packet_size, active_pump_iso_pkts_per_xfer(pump),
		     (unsigned)d->iso_interval, (unsigned)d->payload_size);
		return 0;
	}

	if (failed_at_submit)
		*failed_at_submit = true;
	d->vs_alt = 0;
	d->iso_packet_size = 0;
	d->iso_interval = 0;
	if (pump_err == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
		return UVC_PUMP_ERR_CANCEL_TIMEOUT;
	const int s0 = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (s0 == LIBUSB_ERROR_NO_DEVICE || s0 == LIBUSB_ERROR_ACCESS)
		return s0;
	return pump_err ? pump_err : LIBUSB_ERROR_OTHER;
}

/* Selects and activates an Isochronous altsetting and starts initial ISO URBs
 * after COMMIT. Scans qualifying altsettings (>= payload_size) forward in
 * ascending order when firmware reports a nonzero payload_size. Refuses
 * altsettings smaller than payload_size so the caller can re-negotiate a
 * lower-bandwidth video mode when either SET_INTERFACE or initial ISO URB
 * submission fails due to USB bandwidth limits. */
static int uvc_activate_iso_alt(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump,
				bool *bandwidth_exceeded)
{
	if (bandwidth_exceeded)
		*bandwidth_exceeded = false;
	if (!pump)
		return LIBUSB_ERROR_INVALID_PARAM;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (d->n_iso_alts <= 0)
		return LIBUSB_ERROR_NOT_SUPPORTED;

	const uint32_t effective_payload = (d->quirks & QUIRK_ISO_IGNORE_PAYLOAD) ? 0 : d->payload_size;
	bool fallback_largest = false;
	int idx = uvc_iso_pick_alt(d->iso_alts, d->n_iso_alts, effective_payload, &fallback_largest);
	if (idx < 0) {
		if (bandwidth_exceeded)
			*bandwidth_exceeded = true;
		const struct uvc_iso_alt *max_alt = &d->iso_alts[d->n_iso_alts - 1];
		blog(LOG_WARNING,
		     "uvc: 设备协商 dwMaxPayloadTransferSize=%u 超过最大 ISO alt%d 包长上限 %u，拒绝降档并视为带宽不足",
		     (unsigned)d->payload_size, max_alt->alt_setting, (unsigned)max_alt->max_packet_size);
		return LIBUSB_ERROR_OVERFLOW;
	}

	if (!fallback_largest) {
		int r = LIBUSB_ERROR_NOT_SUPPORTED;
		const int start_idx = idx;
		for (int i = start_idx; i < d->n_iso_alts; i++) {
			if (uvc_stopping(s))
				return LIBUSB_ERROR_INTERRUPTED;
			const struct uvc_iso_alt *alt = &d->iso_alts[i];
			bool failed_at_submit = false;
			r = uvc_try_activate_iso_alt(d, s, pump, alt, &failed_at_submit);
			if (r == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
				return UVC_PUMP_ERR_CANCEL_TIMEOUT;
			if (uvc_stopping(s))
				return LIBUSB_ERROR_INTERRUPTED;
			if (r == 0)
				return 0;
			if (!uvc_iso_is_bandwidth_error(r))
				break;
			if (i + 1 < d->n_iso_alts)
				blog(LOG_WARNING,
				     "uvc: %s alt%d (pkt=%u) 带宽/资源受限（r=%d），继续尝试下一合格档 alt%d (pkt=%u)",
				     failed_at_submit ? "初始 ISO URB 提交" : "SET_INTERFACE", alt->alt_setting,
				     (unsigned)alt->max_packet_size, r, d->iso_alts[i + 1].alt_setting,
				     (unsigned)d->iso_alts[i + 1].max_packet_size);
		}
		if (uvc_iso_is_bandwidth_error(r)) {
			if (bandwidth_exceeded)
				*bandwidth_exceeded = true;
			blog(LOG_WARNING,
			     "uvc: 所有满足协商载荷 %u 的 ISO alt 档均分配/提交失败（r=%d），拒绝降档至更小载荷档位",
			     (unsigned)d->payload_size, r);
		} else {
			blog(LOG_ERROR, "uvc: 激活 ISO 接口替代模式失败（返回 r=%d）", r);
		}
		return r;
	}

	blog(LOG_WARNING,
	     "uvc: %s（payload=%u），回退尝试最大带宽档 alt%d (pkt=%u)",
	     (d->quirks & QUIRK_ISO_IGNORE_PAYLOAD) ? "设备怪癖忽略 dwMaxPayloadTransferSize"
						   : "设备未报告 dwMaxPayloadTransferSize",
	     (unsigned)d->payload_size, d->iso_alts[idx].alt_setting, (unsigned)d->iso_alts[idx].max_packet_size);

	int r = LIBUSB_ERROR_NOT_SUPPORTED;
	for (; idx >= 0; idx--) {
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		const struct uvc_iso_alt *alt = &d->iso_alts[idx];
		bool failed_at_submit = false;
		r = uvc_try_activate_iso_alt(d, s, pump, alt, &failed_at_submit);
		if (r == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
			return UVC_PUMP_ERR_CANCEL_TIMEOUT;
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (r == 0)
			return 0;
		if (uvc_iso_is_bandwidth_error(r)) {
			blog(LOG_WARNING, "uvc: %s alt%d (pkt=%u) 带宽/资源受限（r=%d），尝试降档",
			     failed_at_submit ? "初始 ISO URB 提交" : "SET_INTERFACE", alt->alt_setting,
			     (unsigned)alt->max_packet_size, r);
			continue;
		}
		break;
	}
	if (uvc_iso_is_bandwidth_error(r) && bandwidth_exceeded)
		*bandwidth_exceeded = true;
	blog(LOG_ERROR, "uvc: 激活 ISO 接口替代模式失败（最后返回 r=%d）", r);
	return r;
}

/* Picks the next lower-bandwidth mode for automatic ISO bandwidth downgrade:
 * Tier 1: switch uncompressed -> same/lower-area MJPEG;
 * Tier 2: step down resolution (preferring MJPEG over uncompressed);
 * Tier 3: switch same-area raw format to one with fewer bytes per frame. */
static int uvc_iso_pick_downgrade_mode(const struct uvc_dev *d, const bool *tried, int cur_idx)
{
	if (cur_idx < 0 || cur_idx >= d->n_modes)
		return -1;
	const struct uvc_mode *cur = &d->modes[cur_idx];
	const uint32_t cur_area = (uint32_t)cur->w * (uint32_t)cur->h;
	const bool allow_mjpeg = !(d->quirks & QUIRK_PREFER_YUYV);

	/* Tier 1: non-MJPEG -> MJPEG with area <= cur_area (largest area first). */
	if (cur->kind != MODE_MJPEG && allow_mjpeg) {
		int best = -1;
		uint32_t best_area = 0;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (tried[i] || m->kind != MODE_MJPEG || !mode_is_usable(d->quirks, m->kind))
				continue;
			const uint32_t area = (uint32_t)m->w * (uint32_t)m->h;
			if (area <= cur_area && (best < 0 || area > best_area)) {
				best = i;
				best_area = area;
			}
		}
		if (best >= 0)
			return best;
	}

	/* Tier 2: smaller resolution (largest < cur_area; prefer MJPEG if allowed). */
	{
		int best = -1;
		uint32_t best_area = 0;
		bool best_mj = false;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (tried[i] || !mode_is_usable(d->quirks, m->kind))
				continue;
			const uint32_t area = (uint32_t)m->w * (uint32_t)m->h;
			if (area >= cur_area)
				continue;
			const bool is_mj = allow_mjpeg && m->kind == MODE_MJPEG;
			if (best < 0 || area > best_area || (area == best_area && is_mj && !best_mj)) {
				best = i;
				best_area = area;
				best_mj = is_mj;
			}
		}
		if (best >= 0)
			return best;
	}

	/* Tier 3: same/smaller area with strictly fewer frame bytes (e.g. RGB24/YUYV -> NV12). */
	const uint32_t cur_raw = raw_frame_bytes(cur);
	if (cur_raw > 0) {
		int best = -1;
		uint32_t best_bytes = cur_raw;
		for (int i = 0; i < d->n_modes; i++) {
			const struct uvc_mode *m = &d->modes[i];
			if (tried[i] || !mode_is_usable(d->quirks, m->kind))
				continue;
			const uint32_t area = (uint32_t)m->w * (uint32_t)m->h;
			const uint32_t rb = raw_frame_bytes(m);
			if (area <= cur_area && rb > 0 && rb < best_bytes) {
				best = i;
				best_bytes = rb;
			}
		}
		return best;
	}
	return -1;
}

/* Negotiates video mode + activates ISO altsetting (including initial ISO URB
 * submission), re-negotiating slower FPS or lower-bandwidth modes in automatic
 * mode when USB ISO bandwidth fails. */
static int uvc_iso_negotiate_and_activate(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump,
					  bool *bandwidth_exceeded)
{
	if (bandwidth_exceeded)
		*bandwidth_exceeded = false;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;

	bool bw_err = false;
	int r = uvc_negotiate(d, s, true);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (r != 0)
		return r;
	r = uvc_activate_iso_alt(d, s, pump, &bw_err);
	if (r == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
		return UVC_PUMP_ERR_CANCEL_TIMEOUT;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (r == 0 || !bw_err)
		return r;

	const bool mode_locked =
		s->requested_interval != 0 || raw_low_latency(s) || is_rgb24(d->modes[d->chosen].kind);
	if (mode_locked) {
		if (bandwidth_exceeded)
			*bandwidth_exceeded = true;
		if (!uvc_stopping(s))
			libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
		blog(LOG_ERROR, "uvc: 已锁定模式所需 ISO 带宽不足（payload=%u，r=%d），请手动降低分辨率或帧率",
		     (unsigned)d->payload_size, r);
		return r;
	}

	bool tried[MAX_MODES] = {0};
	uint32_t cur_iv = d->frame_interval ? d->frame_interval : pick_interval(&d->modes[d->chosen], d->eff_fps);
	int last_bw_err = r;

	for (int attempt = 0; attempt < 32; attempt++) {
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		const struct uvc_mode *cur = &d->modes[d->chosen];
		const uint32_t next_iv = uvc_iso_next_slower_interval(cur_iv, cur->intervals, cur->n_intervals,
									     cur->min_iv, cur->max_iv, cur->step_iv);
		if (next_iv > 0) {
			d->iso_override_interval = next_iv;
			cur_iv = next_iv;
		} else {
			tried[d->chosen] = true;
			const int next_idx = uvc_iso_pick_downgrade_mode(d, tried, d->chosen);
			if (next_idx < 0)
				break;
			d->chosen = next_idx;
			d->iso_override_interval = 0;
			cur_iv = 0;
		}

		const int s0 = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (s0 != 0)
			return s0;

		const int req_chosen = d->chosen;
		r = uvc_negotiate(d, s, true);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (r == LIBUSB_ERROR_NOT_SUPPORTED) {
			blog(LOG_WARNING, "uvc: 降级候选模式被设备拒绝或返回未知/不可用索引，继续尝试下一候选");
			if (d->iso_override_interval == 0)
				tried[req_chosen] = true;
			continue;
		}
		if (r != 0)
			return r;

		bw_err = false;
		r = uvc_activate_iso_alt(d, s, pump, &bw_err);
		if (r == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
			return UVC_PUMP_ERR_CANCEL_TIMEOUT;
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (r == 0) {
			const struct uvc_mode *chosen = &d->modes[d->chosen];
			blog(LOG_WARNING,
			     "uvc: ISO 带宽不足，已自动降级为 %s %ux%u @ ≈%.2f fps（alt%d pkt=%u payload=%u）",
			     mode_kind_name(chosen->kind), (unsigned)chosen->w, (unsigned)chosen->h,
			     d->frame_interval ? 10000000.0 / d->frame_interval : 0.0, d->vs_alt,
			     (unsigned)d->iso_packet_size, (unsigned)d->payload_size);
			return 0;
		}
		if (!bw_err)
			return r;

		last_bw_err = r;
		if (d->chosen != req_chosen) {
			tried[req_chosen] = true;
			if (tried[d->chosen])
				cur_iv = 2000000U;
			else
				cur_iv = d->frame_interval ? d->frame_interval
							   : pick_interval(&d->modes[d->chosen], d->eff_fps);
		} else {
			if (d->frame_interval > cur_iv)
				cur_iv = d->frame_interval;
			if (cur_iv == 0)
				cur_iv = pick_interval(&d->modes[d->chosen], d->eff_fps);
		}
	}

	if (bandwidth_exceeded)
		*bandwidth_exceeded = true;
	if (!uvc_stopping(s))
		libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
	blog(LOG_ERROR, "uvc: 所有兼容视频模式均无法在当前 USB 控制器下分配到足够 ISO 带宽（最后 r=%d）",
	     last_bw_err);
	return last_bw_err;
}

/* ---------------- 帧送出 ---------------- */

static const char *tj_subsamp_name(int s)
{
	switch (s) {
	case TJSAMP_444:
		return "444";
	case TJSAMP_422:
		return "422";
	case TJSAMP_420:
		return "420";
	case TJSAMP_GRAY:
		return "gray";
	case TJSAMP_440:
		return "440";
	case TJSAMP_411:
		return "411";
	default:
		return "?";
	}
}

static const char *tj_cs_name(int c)
{
	switch (c) {
	case TJCS_RGB:
		return "RGB";
	case TJCS_YCbCr:
		return "YCbCr";
	case TJCS_GRAY:
		return "GRAY";
	case TJCS_CMYK:
		return "CMYK";
	case TJCS_YCCK:
		return "YCCK";
	default:
		return "?";
	}
}

/* 黑场判定（黑场自降级看门狗用）：步长 64 字节采样，任一采样点亮度/通道 ≥30 = 有内容。
 * bpp=4 → BGRA（B/G/R 任一）;bpp=1 → YUYV 的 Y（偶数字节，64 步长全落偶数）或
 * NV12 的 Y 平面（调用方把 len 限在 Y 平面内，别采到 UV 的 128 误判有内容） */
static bool frame_is_black(const uint8_t *buf, size_t len, int bpp)
{
	for (size_t p = 0; p < len; p += 64) {
		if (bpp == 4) {
			if (buf[p] >= 30 || (p + 1 < len && buf[p + 1] >= 30) || (p + 2 < len && buf[p + 2] >= 30))
				return false;
		} else if (buf[p] >= 30) {
			return false;
		}
	}
	return true;
}

static uint64_t frame_timestamp(struct uvc_source *s, uint64_t received_ns);

static void push_frame_bgra(struct uvc_source *s, uint8_t *bgra, int w, int h, uint64_t received_ns)
{
	struct obs_source_frame out = {0};
	out.width = (uint32_t)w;
	out.height = (uint32_t)h;
	out.format = VIDEO_FORMAT_BGRA;
	out.data[0] = bgra;
	out.linesize[0] = (uint32_t)w * 4;
	out.full_range = true;
	out.timestamp = frame_timestamp(s, received_ns);
	/* YUV 着色器无条件读 color_matrix/range 上下限，不填就是纯黑（仓内先例 v4l2-input.c:128） */
	video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, VIDEO_RANGE_FULL, VIDEO_FORMAT_BGRA, out.color_matrix,
					       out.color_range_min, out.color_range_max);
	obs_source_output_video(s->source, &out);

	/* 像素探针：WSL 复现已实锤——同一调用序列解 splash_raw.jpg 在四个解码器
	 * （libjpeg-turbo 2.1.5/3.1.0 同族、Pillow、ffmpeg 7/8）下都得 B=G=R=7，与
	 * Android 探针一致，即解码忠实、采集卡给的就是黑帧。这里升级成"中心 + 全帧
	 * 均值/峰值"：全帧峰值 < 30 = 卡出黑场（查 HDMI 源/卡状态）；有明显内容但
	 * 预览黑 = 前端显示问题 */
	if (s->frames_out < 3 || (s->frames_out + 1) % 300 == 0) {
		const int cw = w < 64 ? w : 64;
		const int ch = h < 64 ? h : 64;
		uint64_t sb = 0, sg = 0, sr = 0;
		for (int yy = 0; yy < ch; yy++) {
			const uint8_t *row =
				bgra + (size_t)(h / 2 - ch / 2 + yy) * (size_t)w * 4 + (size_t)(w / 2 - cw / 2) * 4;
			for (int xx = 0; xx < cw; xx++) {
				sb += row[xx * 4];
				sg += row[xx * 4 + 1];
				sr += row[xx * 4 + 2];
			}
		}
		const uint64_t n = (uint64_t)cw * (uint64_t)ch;
		uint64_t fb = 0, fg = 0, fr = 0;
		unsigned mb = 0, mg = 0, mr = 0;
		for (size_t p = 0; p < (size_t)w * (size_t)h; p++) {
			const uint8_t b = bgra[p * 4], g = bgra[p * 4 + 1], r = bgra[p * 4 + 2];
			fb += b;
			fg += g;
			fr += r;
			if (b > mb)
				mb = b;
			if (g > mg)
				mg = g;
			if (r > mr)
				mr = r;
		}
		const uint64_t fn = (uint64_t)w * (uint64_t)h;
		const bool black = mb < 30 && mg < 30 && mr < 30;
		blog(LOG_INFO,
		     "uvc: 像素探针 帧#%llu 中心均值 B=%llu G=%llu R=%llu | 全帧均值 B=%llu G=%llu R=%llu "
		     "全帧峰值 B=%u G=%u R=%u%s（showing=%d active=%d）",
		     (unsigned long long)(s->frames_out + 1), (unsigned long long)(sb / n),
		     (unsigned long long)(sg / n), (unsigned long long)(sr / n), (unsigned long long)(fb / fn),
		     (unsigned long long)(fg / fn), (unsigned long long)(fr / fn), mb, mg, mr,
		     black ? " —— 全帧黑：采集卡出的就是黑场，查 HDMI 源/卡" : " —— 有内容",
		     obs_source_showing(s->source) ? 1 : 0, obs_source_active(s->source) ? 1 : 0);
	}
}

static uint64_t frame_timestamp(struct uvc_source *s, uint64_t received_ns)
{
	/* Completion time is a host arrival timestamp, not the HDMI camera clock. */
	const uint64_t raw_ts = received_ns ? received_ns : os_gettime_ns();
	const uint64_t cur_drops = atomic_load_explicit(&s->frames_dropped, memory_order_relaxed);
	if (cur_drops != s->ts_last_frames_dropped) {
		s->ts_last_frames_dropped = cur_drops;
		uvc_ts_smoother_mark_discontinuity(&s->ts_smoother);
	}
	const bool smooth_enabled = !obs_source_async_unbuffered(s->source);
	return uvc_ts_smoother_step(&s->ts_smoother, raw_ts, smooth_enabled);
}

static void expand_rgb24(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint32_t stride)
{
	for (uint32_t row = 0; row < height; ++row) {
		const uint8_t *in = src + (size_t)row * stride;
		uint32_t x = 0;
#if defined(__ARM_NEON)
		const uint8x16_t alpha = vdupq_n_u8(255);
		for (; x + 16 <= width; x += 16, in += 48, dst += 64) {
			const uint8x16x3_t rgb = vld3q_u8(in);
			const uint8x16x4_t rgba = {{rgb.val[0], rgb.val[1], rgb.val[2], alpha}};
			vst4q_u8(dst, rgba);
		}
#endif
		for (; x < width; ++x, in += 3, dst += 4) {
			dst[0] = in[0];
			dst[1] = in[1];
			dst[2] = in[2];
			dst[3] = 255;
		}
	}
}

static bool push_frame_raw(struct uvc_source *s, const struct uvc_mode *m, struct uvc_stream *st, uint32_t len,
			   uint64_t received_ns)
{
	if (len < raw_frame_bytes(m)) {
		s->frames_dropped++;
		return false;
	}
	const bool rgb = is_rgb24(m->kind);
	enum video_format format;
	switch (m->kind) {
	case MODE_YUYV:
		format = VIDEO_FORMAT_YUY2;
		break;
	case MODE_UYVY:
		format = VIDEO_FORMAT_UYVY;
		break;
	case MODE_NV12:
		format = VIDEO_FORMAT_NV12;
		break;
	case MODE_P010:
		format = VIDEO_FORMAT_P010;
		break;
	case MODE_RGB24:
		format = VIDEO_FORMAT_RGBA;
		break;
	case MODE_BGR24:
		format = VIDEO_FORMAT_BGR3;
		break;
	default:
		return false;
	}
	const enum video_range_type range =
		resolve_video_range(format, atomic_load_explicit(&s->color_range, memory_order_relaxed));
	struct obs_source_frame2 *out = &s->raw_frame;
	if (out->format != format || out->width != m->w || out->height != m->h || out->range != range) {
		memset(out, 0, sizeof(*out));
		out->format = format;
		out->width = m->w;
		out->height = m->h;
		out->range = range;
		enum video_colorspace space = rgb ? VIDEO_CS_SRGB : s->colorspace;
		if (space == VIDEO_CS_2100_PQ || space == VIDEO_CS_2100_HLG) {
			if (format == VIDEO_FORMAT_P010)
				out->trc = space == VIDEO_CS_2100_PQ ? VIDEO_TRC_PQ : VIDEO_TRC_HLG;
			else
				space = VIDEO_CS_709;
		}
		video_format_get_parameters_for_format(space, range, format, out->color_matrix, out->color_range_min,
						       out->color_range_max);
	}
	out->data[0] = st->frame_buf;
	if (rgb) {
		out->linesize[0] = rgb24_stride(m);
		if (m->kind == MODE_RGB24) {
			expand_rgb24(st->frame_buf, st->rgba_buf, m->w, m->h, out->linesize[0]);
			out->data[0] = st->rgba_buf;
			out->linesize[0] = (uint32_t)m->w * 4;
		}
	} else {
		out->linesize[0] = (uint32_t)m->w * (m->kind == MODE_NV12 ? 1U : 2U);
		if (m->kind == MODE_NV12 || m->kind == MODE_P010) {
			out->data[1] = st->frame_buf + (size_t)out->linesize[0] * m->h;
			out->linesize[1] = out->linesize[0];
		}
		if (m->kind != MODE_P010 && m->kind != MODE_UYVY)
			uvc_cadence_observe(&s->cadence, st->frame_buf, m->w, m->h, m->kind == MODE_YUYV, received_ns);
	}
	out->timestamp = frame_timestamp(s, received_ns);
	/* The legacy output API forces full range on RGB frames. */
	obs_source_output_video2(s->source, out);
	return true;
}

/* ---------------- bulk 字节流解析（同步锁定 + 帧重组） ---------------- */

static void sbuf_consume(struct uvc_stream *st, size_t n)
{
	uvc_stream_buffer_consume(&st->input, n);
}

/* UVC 规范里头长只能是 2(基)+4(PTS)+6(SCR) 的组合：2/6/8/12，其余（比如 5）不可能。
 * 上一版就死在把 JPEG 熵数据里 HLE=5、周期 196 字节的巧合模式当成了真同步。 */
static bool hle_valid(uint8_t hle)
{
	return hle == 2 || hle == 6 || hle == 8 || hle == 12;
}

/* 循环内日志统一限频：每秒最多放行 1 行（日志 IO 拖慢 bulk 读 → 设备缓冲溢出 →
 * 更多 ERR 的死亡螺旋，真机第五轮实录 7 万行） */
static bool log_allow(uint64_t *slot)
{
	const uint64_t now = os_gettime_ns();
	if (now - *slot < 1000000000ULL)
		return false;
	*slot = now;
	return true;
}

/* 从 scan_off 起找下一个候选头起点：HLE 是同步时学会的那个（未同步时认白名单），
 * BMH bit7(EOH)=1、bit4(保留)=0。找不到返回 SIZE_MAX。只扫不判——候选是不是真的头，
 * 由状态机用 FID/PTS 一致性裁定（随机数据装出这两字节的概率约 1/1024，防不住，
 * 但撞 PTS 约 2^-32，帧内 PTS 一致性才是真闸门）。
 * 未同步找锚点时额外要求 EOF=0 且 ERR=0（真机假锚实录：0xA3 带 EOF、0xCC 带 ERR，
 * 都不是正常帧中头，不能拿来学头长）。 */
static size_t scan_candidate(const struct uvc_stream *st)
{
	for (size_t i = st->scan_off; i + 2 <= st->input.size; i++) {
		const uint8_t b0 = st->input.data[i];
		const uint8_t b1 = st->input.data[i + 1];
		const bool empty_raw_eof = b0 == 2 && st->assembling && st->frame_total &&
					   st->frame_len + i == st->frame_total && (b1 & UVC_BMH_EOF);
		if (!(st->hle ? b0 == st->hle || empty_raw_eof : hle_valid(b0)) || (b1 & 0x90) != 0x80)
			continue;
		const uint8_t expected_hle = 2 + ((b1 & 0x04) ? 4 : 0) + ((b1 & 0x08) ? 6 : 0);
		if (b0 != expected_hle)
			continue;
		if (!st->hle && (b1 & (UVC_BMH_ERR | UVC_BMH_EOF)) != 0)
			continue;
		return i;
	}
	return SIZE_MAX;
}

/* 送帧校验用：在帧尾 FRAME_TAIL_SEARCH 窗口里往回找 FFD9（允许尾部少量填充），
 * 返回 EOI 后一字节的偏移；找不到返回 0。 */
static size_t find_eoi_tail(const uint8_t *p, size_t len)
{
	const size_t start = len > FRAME_TAIL_SEARCH ? len - FRAME_TAIL_SEARCH : 0;
	for (size_t q = len - 1; q > start; q--) {
		if (p[q - 1] == 0xFF && p[q] == 0xD9)
			return q + 1;
	}
	return 0;
}

/* 诊断 dump：流位置前 64 字节 + 当前帧缓冲开头 32 字节（看帧首是不是 FF D8，
 * 一眼判断数据对齐没有）。每轮采集只打第一次——上一版用"连续失步 N 次"触发，
 * 但假同步场景下每次失步后都能再假锁、计数器清零，永远到不了 N。 */
static void dump_diag(struct uvc_stream *st)
{
	if (st->diag_dumped)
		return;
	st->diag_dumped = true;

	const size_t n = st->input.size < 64 ? st->input.size : 64;
	char line[64 * 3 + 1];
	line[0] = '\0'; /* n==0 时 loop 不跑，没这行 blog("%s") 读未初始化栈 */
	for (size_t i = 0; i < n; i++)
		snprintf(line + i * 3, 4, "%02x ", st->input.data[i]);
		blog(LOG_WARNING, "uvc: 同步异常，缓冲区起始 %zu 字节：%s", n, line);

	const size_t fn = st->frame_len < 32 ? st->frame_len : 32;
	char fline[32 * 3 + 1];
	fline[0] = '\0';
	for (size_t i = 0; i < fn; i++)
		snprintf(fline + i * 3, 4, "%02x ", st->frame_buf[i]);
	const char *soi_note = "";
	if (st->frame_total == 0)
		soi_note = fn >= 2 && st->frame_buf[0] == 0xFF && st->frame_buf[1] == 0xD8 ? "（帧首 FFD8 ✓）"
											    : "（帧首非 FFD8 ✗）";
	blog(LOG_WARNING, "uvc: 失步诊断，帧缓冲已攒 %zu 字节，开头 %zu 字节：%s%s", st->frame_len, fn, fline, soi_note);
}

/* 重同步：作废当前半帧、扫描游标归零，但坚持已学会的会话头长（真机锁到过
 * HLE=8/BMH=0xA3、0xCC 的假锚，回退白名单会再次假锁）；连假 5 次才连头长一起重学 */
static void stream_resync(struct uvc_stream *st, const char *why)
{
	st->resync_streak++;
	const bool relearn = st->resync_streak % 5 == 0;
	if (log_allow(&st->ll_resync))
		blog(LOG_WARNING, "uvc: 触发流重同步（原因=%s），丢弃半帧（已累积 %zu 字节）%s", why, st->frame_len,
		     relearn ? "，连续失败已达阈值，重置包头长度识别" : "");
	if (relearn)
		st->hle = 0;
	st->scan_off = 0;
	st->assembling = false;
	st->frame_len = 0;
	st->has_pts = false;
	st->rejected = 0;
	if (st->resync_streak == RESYNC_DUMP_AFTER)
		dump_diag(st);
}

/* 重新协商/抢救后重置解析状态：设备重开流，但头长是设备属性（这台 = 12），保留；
 * 新会话 st = {0} 天然 hle = 0 从头学 */
static void stream_reset_parse(struct uvc_stream *st)
{
	uvc_stream_buffer_reset(&st->input);
	st->scan_off = 0;
	st->sync_read = 0;
	st->assembling = false;
	st->frame_len = 0;
	st->has_pts = false;
	st->rejected = 0;
	st->bad_streak = 0;
	st->err_first_ns = 0;
}

/* BMH ERR 处理（对齐 Linux uvcvideo：静默丢帧、全速继续读 drain，绝不逐条日志，
 * 也绝不做 SET_INTERFACE/重新协商等动作——廉价 MS2130 类固件会被反复 alt 切换
 * 踢死（真机 1617 实录：踢脚后 IN 路径全 -1 猝死），设备通常自己缓过来）。
 * 持续超 10 秒拆会话重开（新会话协商失败时有 reset_device 兜底）。
 * 返回 true = 调用方必须立刻 break 出流循环拆会话 */
static bool handle_err_header(struct uvc_source *s, struct uvc_stream *st)
{
	st->err_count++;
	const uint64_t now = os_gettime_ns();
	if (!st->err_first_ns)
		st->err_first_ns = now;
	const uint64_t dur = now - st->err_first_ns;

	if (log_allow(&st->ll_err))
		blog(LOG_WARNING, "uvc: 设备报流错误（BMH ERR），静默丢帧全速读中（累计 %llu 次，本段持续 %.1f 秒）",
		     (unsigned long long)st->err_count, dur / 1e9);

	if (dur >= 10000000000ULL) {
		blog(LOG_ERROR, "uvc: BMH ERR 持续超 10 秒，拆会话重开");
		set_status_key(s, "Status.StreamErrorRestart", NULL, 0);
		return true;
	}
	return false;
}

/* ---------------- 一轮完整采集（拿 fd → detach → 协商 → bulk 流循环） ---------------- */

static bool ensure_bulk_stream_buffer(struct uvc_stream *st)
{
	if (st->input.storage)
		return true;
	st->input.capacity = SBUF_HIGH_WATER + UVC_BULK_BATCH_TARGET + 64;
	st->input.storage = bmalloc(st->input.capacity);
	if (!st->input.storage)
		return false;
	uvc_stream_buffer_reset(&st->input);
	return true;
}

static bool use_raw_packets(const struct uvc_dev *d, const struct uvc_source *s)
{
	UNUSED_PARAMETER(s);
	if (d->is_iso || d->bulk_raw_fallback || d->chosen < 0 || d->chosen >= d->n_modes)
		return false;
	return raw_frame_bytes(&d->modes[d->chosen]) > 0 && !(d->quirks & QUIRK_PREFER_YUYV) &&
	       d->payload_size >= 2 && d->payload_size <= UVC_BULK_CHUNK_SIZE;
}

/* Standard UVC Bulk Payload Transfer framing for devices whose negotiated
 * dwMaxPayloadTransferSize exceeds one 64 KiB host URB (e.g. Sony A7M4/ZV-E10
 * sending each MJPEG frame as one 1.38 MB bulk payload terminated by a short
 * packet or ZLP). Small-payload capture cards (e.g. MS2130) stay on the
 * byte-stream scanner or fixed raw-payload fast path. */
static bool use_bulk_payload_assembler(const struct uvc_dev *d, const struct uvc_source *s)
{
	if (d->is_iso || d->bulk_payload_fallback || d->chosen < 0 || d->chosen >= d->n_modes)
		return false;
	if (use_raw_packets(d, s))
		return false;
	return d->payload_size > UVC_BULK_CHUNK_SIZE;
}

struct uvc_active_pump {
	struct uvc_bulk_pump *bulk;
	struct uvc_iso_pump *iso;
	struct uvc_bulk_pump *retained_bulk;
	struct uvc_iso_pump *retained_iso;
	unsigned iso_pkts_per_xfer;
	bool cancel_timed_out;
};

static bool uvc_retire_step_deadline(bool monotonic, struct timespec *ts, uint32_t timeout_ms)
{
	if (clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, ts) != 0)
		return false;
	ts->tv_sec += timeout_ms / 1000U;
	ts->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
	return true;
}

static void uvc_retire_gate_release(struct uvc_retire_gate *g)
{
	if (!g)
		return;
	pthread_mutex_lock(&g->mtx);
	const bool last = (g->refs > 0 && --g->refs == 0);
	pthread_mutex_unlock(&g->mtx);
	if (last) {
		pthread_cond_destroy(&g->cond);
		pthread_mutex_destroy(&g->mtx);
		free(g);
	}
}

/* Lock order is always s->mtx -> g->mtx, never the reverse. */
static struct uvc_retire_gate *uvc_source_retain_retire_gate(struct uvc_source *s, bool create)
{
	if (!s)
		return NULL;
	pthread_mutex_lock(&s->mtx);
	struct uvc_retire_gate *g = s->retire_gate;
	if (!g && create) {
		g = calloc(1, sizeof(*g));
		if (g) {
			if (pthread_mutex_init(&g->mtx, NULL) != 0) {
				free(g);
				g = NULL;
			} else {
				pthread_condattr_t attr;
				const bool have_attr = pthread_condattr_init(&attr) == 0;
				bool cond_ok = false;
				if (have_attr && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
					g->monotonic_cond = true;
					cond_ok = pthread_cond_init(&g->cond, &attr) == 0;
				} else {
					cond_ok = pthread_cond_init(&g->cond, NULL) == 0;
				}
				if (have_attr)
					pthread_condattr_destroy(&attr);
				if (!cond_ok) {
					pthread_mutex_destroy(&g->mtx);
					free(g);
					g = NULL;
				} else {
					g->refs = 1;
					s->retire_gate = g;
				}
			}
		}
	}
	if (g) {
		pthread_mutex_lock(&g->mtx);
		g->refs++;
		pthread_mutex_unlock(&g->mtx);
	}
	pthread_mutex_unlock(&s->mtx);
	return g;
}

static void uvc_source_release_retire_gate(struct uvc_source *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->mtx);
	struct uvc_retire_gate *g = s->retire_gate;
	s->retire_gate = NULL;
	pthread_mutex_unlock(&s->mtx);
	uvc_retire_gate_release(g);
}

static void uvc_retire_gate_signal(struct uvc_source *s)
{
	struct uvc_retire_gate *g = uvc_source_retain_retire_gate(s, false);
	if (!g)
		return;
	pthread_mutex_lock(&g->mtx);
	pthread_cond_broadcast(&g->cond);
	pthread_mutex_unlock(&g->mtx);
	uvc_retire_gate_release(g);
}

static bool uvc_retire_is_pending(struct uvc_source *s)
{
	struct uvc_retire_gate *g = uvc_source_retain_retire_gate(s, false);
	if (!g)
		return false;
	pthread_mutex_lock(&g->mtx);
	const bool pending = g->active_jobs > 0;
	pthread_mutex_unlock(&g->mtx);
	uvc_retire_gate_release(g);
	return pending;
}

static bool uvc_retire_wait_idle(struct uvc_source *s, unsigned max_wait_ms)
{
	struct uvc_retire_gate *g = uvc_source_retain_retire_gate(s, false);
	if (!g)
		return !uvc_stopping(s);

	const uint64_t end_ns = os_gettime_ns() + (uint64_t)max_wait_ms * 1000000ULL;
	pthread_mutex_lock(&g->mtx);
	while (g->active_jobs > 0 && !uvc_stopping(s)) {
		const uint64_t now = os_gettime_ns();
		if (now >= end_ns)
			break;
		const uint64_t rem_ms = (end_ns - now + 999999ULL) / 1000000ULL;
		const unsigned step_ms = rem_ms > 20U ? 20U : (unsigned)rem_ms;
		struct timespec ts;
		if (uvc_retire_step_deadline(g->monotonic_cond, &ts, step_ms))
			pthread_cond_timedwait(&g->cond, &g->mtx, &ts);
		else
			break;
	}
	const bool idle = (g->active_jobs == 0) && !uvc_stopping(s);
	pthread_mutex_unlock(&g->mtx);
	uvc_retire_gate_release(g);
	return idle;
}

static void uvc_release_and_restore_interfaces(libusb_device_handle *h, bool vs_claimed, bool is_iso, int vs_iface,
					       int vc_iface, bool vc_detached, bool vs_detached, bool auto_detach_ok)
{
	if (!h)
		return;
	if (vs_claimed) {
		if (is_iso)
			libusb_set_interface_alt_setting(h, vs_iface, 0);
		libusb_release_interface(h, vs_iface);
	}
	if (vs_detached && (!vs_claimed || !auto_detach_ok)) {
		const int ar = libusb_attach_kernel_driver(h, vs_iface);
		blog(LOG_INFO, "uvc: 恢复 VS 接口 %d 内核驱动 r=%d", vs_iface, ar);
	}
	if (vc_detached) {
		const int ar = libusb_attach_kernel_driver(h, vc_iface);
		blog(LOG_INFO, "uvc: 恢复 VC 接口 %d 内核驱动 r=%d", vc_iface, ar);
	}
}

struct uvc_retire_job {
	struct uvc_retire_gate *gate;
	struct uvc_bulk_pump *retained_bulk;
	struct uvc_iso_pump *retained_iso;
	libusb_context *ctx;
	libusb_device_handle *h;
	int bridge_fd;
	int vs_iface;
	int vc_iface;
	bool vs_claimed;
	bool is_iso;
	bool vc_detached;
	bool vs_detached;
	bool auto_detach_ok;
};

/* Give slow-cancelling URBs up to 2 s on the detached retire thread so libusb
 * can reap them cleanly and run op_clear_transfer_priv (freeing Linux usbfs
 * tpriv->urbs / iso_urbs). Persistent event errors still break out immediately
 * via DRAIN_EVENT_ERROR_LIMIT, accepting the small (~2-4 KB) libusb 1.0.28
 * backend URB metadata leak rather than hanging UI teardown or risking UAF. */
#define UVC_RETIRE_ASYNC_DRAIN_MS 2000U
#define UVC_RETIRE_OOM_DRAIN_MS 100U

static void uvc_retire_execute(struct uvc_retire_job *job)
{
	bool drained = true;
	if (job->retained_bulk)
		drained &= uvc_bulk_pump_try_drain(job->retained_bulk, UVC_RETIRE_ASYNC_DRAIN_MS);
	if (job->retained_iso)
		drained &= uvc_iso_pump_try_drain(job->retained_iso, UVC_RETIRE_ASYNC_DRAIN_MS);
	if (drained && job->h)
		uvc_release_and_restore_interfaces(job->h, job->vs_claimed, job->is_iso, job->vs_iface, job->vc_iface,
						   job->vc_detached, job->vs_detached, job->auto_detach_ok);
	/* Close handle and underlying fd before freeing transfers/buffers so libusb
	 * and kernel usbfs stop touching those buffers before exit(ctx). Note: in
	 * libusb 1.0.28, do_close() + libusb_free_transfer() do not invoke Linux
	 * op_clear_transfer_priv() if drained == false; only catastrophic event-loop
	 * failure reaches that forced-close fallback. */
	if (job->h)
		libusb_close(job->h);
	if (job->bridge_fd >= 0)
		obs_android_usb_close(job->bridge_fd);
	if (job->retained_bulk)
		uvc_bulk_pump_reclaim_after_close(job->retained_bulk);
	if (job->retained_iso)
		uvc_iso_pump_reclaim_after_close(job->retained_iso);
	if (job->ctx)
		libusb_exit(job->ctx);

	struct uvc_retire_gate *g = job->gate;
	free(job);
	if (g) {
		pthread_mutex_lock(&g->mtx);
		if (g->active_jobs > 0)
			g->active_jobs--;
		pthread_cond_broadcast(&g->cond);
		pthread_mutex_unlock(&g->mtx);
		uvc_retire_gate_release(g);
	}
}

static void *uvc_retire_thread_main(void *arg)
{
	uvc_retire_execute((struct uvc_retire_job *)arg);
	return NULL;
}

static void uvc_retire_session_async(struct uvc_source *s, struct uvc_bulk_pump *retained_bulk,
				     struct uvc_iso_pump *retained_iso, libusb_context *ctx, libusb_device_handle *h,
				     int bridge_fd, bool vs_claimed, bool is_iso, int vs_iface, int vc_iface,
				     bool vc_detached, bool vs_detached, bool auto_detach_ok)
{
	struct uvc_retire_job *job = calloc(1, sizeof(*job));
	if (!job) {
		struct uvc_retire_job stack_job = {
			.gate = NULL,
			.retained_bulk = retained_bulk,
			.retained_iso = retained_iso,
			.ctx = ctx,
			.h = h,
			.bridge_fd = bridge_fd,
			.vs_iface = vs_iface,
			.vc_iface = vc_iface,
			.vs_claimed = vs_claimed,
			.is_iso = is_iso,
			.vc_detached = vc_detached,
			.vs_detached = vs_detached,
			.auto_detach_ok = auto_detach_ok,
		};
		/* Inline fallback on OOM: run stack job directly via execute? Do not free stack! */
		bool drained = true;
		if (stack_job.retained_bulk)
			drained &= uvc_bulk_pump_try_drain(stack_job.retained_bulk, UVC_RETIRE_OOM_DRAIN_MS);
		if (stack_job.retained_iso)
			drained &= uvc_iso_pump_try_drain(stack_job.retained_iso, UVC_RETIRE_OOM_DRAIN_MS);
		if (drained && stack_job.h)
			uvc_release_and_restore_interfaces(stack_job.h, stack_job.vs_claimed, stack_job.is_iso,
							   stack_job.vs_iface, stack_job.vc_iface,
							   stack_job.vc_detached, stack_job.vs_detached,
							   stack_job.auto_detach_ok);
		if (stack_job.h)
			libusb_close(stack_job.h);
		if (stack_job.bridge_fd >= 0)
			obs_android_usb_close(stack_job.bridge_fd);
		if (stack_job.retained_bulk)
			uvc_bulk_pump_reclaim_after_close(stack_job.retained_bulk);
		if (stack_job.retained_iso)
			uvc_iso_pump_reclaim_after_close(stack_job.retained_iso);
		if (stack_job.ctx)
			libusb_exit(stack_job.ctx);
		return;
	}

	struct uvc_retire_gate *g = uvc_source_retain_retire_gate(s, true);
	job->gate = g;
	job->retained_bulk = retained_bulk;
	job->retained_iso = retained_iso;
	job->ctx = ctx;
	job->h = h;
	job->bridge_fd = bridge_fd;
	job->vs_iface = vs_iface;
	job->vc_iface = vc_iface;
	job->vs_claimed = vs_claimed;
	job->is_iso = is_iso;
	job->vc_detached = vc_detached;
	job->vs_detached = vs_detached;
	job->auto_detach_ok = auto_detach_ok;

	if (g) {
		pthread_mutex_lock(&g->mtx);
		g->active_jobs++;
		pthread_mutex_unlock(&g->mtx);
	}

	pthread_attr_t attr;
	bool have_attr = pthread_attr_init(&attr) == 0;
	if (have_attr)
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t tid;
	const int cr = pthread_create(&tid, have_attr ? &attr : NULL, uvc_retire_thread_main, job);
	if (have_attr)
		pthread_attr_destroy(&attr);
	if (cr != 0)
		uvc_retire_execute(job);
}

struct uvc_pump_view {
	uint64_t received_bytes, dropped_blocks, transfers, errors, packet_errors;
	uint64_t timeouts, stalls, overflows, io_errors;
	unsigned ready_high_water, ready_capacity, active_xfers;
};

static bool active_pump_running(const struct uvc_active_pump *pump)
{
	return pump->iso != NULL || pump->bulk != NULL;
}

static bool active_pump_cancel_timed_out(const struct uvc_active_pump *pump)
{
	return pump && (pump->cancel_timed_out || pump->retained_bulk || pump->retained_iso);
}

static unsigned active_pump_iso_pkts_per_xfer(const struct uvc_active_pump *pump)
{
	return pump ? pump->iso_pkts_per_xfer : 0;
}

static int active_pump_error(const struct uvc_active_pump *pump)
{
	if (pump->iso)
		return uvc_iso_pump_error(pump->iso);
	if (pump->bulk)
		return uvc_bulk_pump_error(pump->bulk);
	return 0;
}

static void active_pump_snapshot(const struct uvc_active_pump *pump, struct uvc_pump_view *view)
{
	memset(view, 0, sizeof(*view));
	if (pump->iso) {
		struct uvc_iso_stats s;
		uvc_iso_pump_snapshot(pump->iso, &s);
		view->received_bytes = s.received_bytes;
		view->dropped_blocks = s.dropped_blocks;
		view->transfers = s.transfers;
		view->errors = s.errors;
		view->packet_errors = s.packet_errors;
		view->timeouts = s.timeouts;
		view->stalls = s.stalls;
		view->overflows = s.overflows;
		view->io_errors = s.io_errors;
		view->ready_high_water = s.ready_high_water;
		view->ready_capacity = s.ready_capacity;
		view->active_xfers = s.active_xfers;
	} else if (pump->bulk) {
		struct uvc_bulk_stats s;
		uvc_bulk_pump_snapshot(pump->bulk, &s);
		view->received_bytes = s.received_bytes;
		view->dropped_blocks = s.dropped_blocks;
		view->transfers = s.transfers;
		view->errors = s.errors;
		view->timeouts = s.timeouts;
		view->stalls = s.stalls;
		view->overflows = s.overflows;
		view->io_errors = s.io_errors;
		view->ready_high_water = s.ready_high_water;
		view->ready_capacity = s.ready_capacity;
	}
}

static struct uvc_bulk_pump *start_input_pump(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump,
					      int *error)
{
	/* A bulk URB may cover several fixed-size payloads. The consumer reframes
	 * by dwMaxPayloadTransferSize; no short packet or firmware change is needed. */
	if (use_raw_packets(d, s))
		return uvc_bulk_pump_start_payload_ex(d->ctx, d->h, d->ep_in, d->payload_size, error,
						     &pump->retained_bulk);
	return uvc_bulk_pump_start_sized_ex(d->ctx, d->h, d->ep_in, UVC_BULK_CHUNK_SIZE, error, &pump->retained_bulk);
}

static int stop_active_pump(struct uvc_active_pump *pump)
{
	int res = 0;
	if (pump->iso) {
		struct uvc_iso_pump *ret_iso = NULL;
		if (uvc_iso_pump_stop_ex(pump->iso, &ret_iso) == UVC_PUMP_ERR_CANCEL_TIMEOUT || ret_iso) {
			if (ret_iso)
				pump->retained_iso = ret_iso;
			pump->cancel_timed_out = true;
			res = UVC_PUMP_ERR_CANCEL_TIMEOUT;
		}
		pump->iso = NULL;
	}
	if (pump->bulk) {
		struct uvc_bulk_pump *ret_bulk = NULL;
		if (uvc_bulk_pump_stop_ex(pump->bulk, &ret_bulk) == UVC_PUMP_ERR_CANCEL_TIMEOUT || ret_bulk) {
			if (ret_bulk)
				pump->retained_bulk = ret_bulk;
			pump->cancel_timed_out = true;
			res = UVC_PUMP_ERR_CANCEL_TIMEOUT;
		}
		pump->bulk = NULL;
	}
	pump->iso_pkts_per_xfer = 0;
	return active_pump_cancel_timed_out(pump) ? UVC_PUMP_ERR_CANCEL_TIMEOUT : res;
}

static bool start_active_pump(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump, int *error)
{
	if (active_pump_cancel_timed_out(pump)) {
		if (error)
			*error = UVC_PUMP_ERR_CANCEL_TIMEOUT;
		return false;
	}
	if (d->is_iso) {
		const struct uvc_mode *m = &d->modes[d->chosen];
		const unsigned pkts = uvc_iso_packets_per_transfer(d->iso_packet_size, d->usb_speed, raw_frame_bytes(m));
		pump->iso = uvc_iso_pump_start_ex(d->ctx, d->h, d->ep_in, d->iso_packet_size, pkts, error,
						  &pump->retained_iso);
		pump->iso_pkts_per_xfer = pump->iso ? pkts : 0;
		if (!pump->iso && ((error && *error == UVC_PUMP_ERR_CANCEL_TIMEOUT) || pump->retained_iso)) {
			pump->cancel_timed_out = true;
			if (error)
				*error = UVC_PUMP_ERR_CANCEL_TIMEOUT;
		}
		return pump->iso != NULL;
	}
	pump->bulk = start_input_pump(d, s, pump, error);
	if (!pump->bulk && ((error && *error == UVC_PUMP_ERR_CANCEL_TIMEOUT) || pump->retained_bulk)) {
		pump->cancel_timed_out = true;
		if (error)
			*error = UVC_PUMP_ERR_CANCEL_TIMEOUT;
	}
	return pump->bulk != NULL;
}

static int restart_active_pump(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump)
{
	if (stop_active_pump(pump) == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
		return UVC_PUMP_ERR_CANCEL_TIMEOUT;
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (d->is_iso) {
		const int sr = libusb_set_interface_alt_setting(d->h, d->vs_iface, 0);
		if (uvc_stopping(s))
			return LIBUSB_ERROR_INTERRUPTED;
		if (sr == LIBUSB_ERROR_NO_DEVICE || sr == LIBUSB_ERROR_ACCESS)
			return sr;
		if (sr != 0)
			blog(LOG_WARNING, "uvc: ISO 重启切回 alt0 r=%d，继续尝试协商", sr);
	}
	int result = uvc_negotiate(d, s, false);
	if (uvc_stopping(s))
		return LIBUSB_ERROR_INTERRUPTED;
	if (result != 0)
		return result;
	if (d->is_iso)
		return uvc_activate_iso_alt(d, s, pump, NULL);
	int error = 0;
	return start_active_pump(d, s, pump, &error) ? 0 : error;
}

/* A reset is followed by full close/re-enumeration, never reuse this handle. */
static void reset_before_reopen(struct uvc_dev *d, struct uvc_source *s, struct uvc_active_pump *pump)
{
	if (stop_active_pump(pump) == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(pump))
		return;
	if (os_atomic_load_long(&s->shutting_down))
		return;
	if (uvc_recovery_allow_reset(&s->reset_limit, os_gettime_ns())) {
		const int rr = libusb_reset_device(d->h);
		blog(LOG_WARNING, "uvc: USB reset r=%d；关闭并重新枚举", rr);
	} else {
		blog(LOG_WARNING, "uvc: USB reset 限频，关闭并重新枚举");
	}
}

/* The USB pump owns submitted transfers; this thread only parses borrowed blocks. */
static aaudio_data_callback_result_t uvc_audio_data(AAudioStream *stream, void *context, void *data, int32_t frames)
{
	struct uvc_source *s = context;
	if (frames <= 0)
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
	const aaudio_format_t aaudio_fmt = AAudioStream_getFormat(stream);
	enum audio_format obs_fmt;
	if (aaudio_fmt == AAUDIO_FORMAT_PCM_I16)
		obs_fmt = AUDIO_FORMAT_16BIT;
	else if (aaudio_fmt == AAUDIO_FORMAT_PCM_FLOAT)
		obs_fmt = AUDIO_FORMAT_FLOAT;
	else
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
	const uint32_t rate = (uint32_t)AAudioStream_getSampleRate(stream);
	if (!rate)
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
	const struct obs_source_audio audio = {
		.data = {(const uint8_t *)data},
		.frames = (uint32_t)frames,
		.speakers = AAudioStream_getChannelCount(stream) == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO,
		.format = obs_fmt,
		.samples_per_sec = rate,
		.timestamp = os_gettime_ns() - (uint64_t)frames * 1000000000ULL / rate,
	};
	obs_source_output_audio(s->source, &audio);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

struct uvc_audio_worker {
	struct uvc_source *s;
	char usb_device[OBS_ANDROID_USB_NAME_LEN];
	pthread_t thread;
	bool started;
	bool sync_init;
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	volatile long stop;
	atomic_bool error_disconnected;
};

static void uvc_audio_worker_signal(struct uvc_audio_worker *w)
{
	if (!w || !w->sync_init)
		return;
	pthread_mutex_lock(&w->mtx);
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mtx);
}

static void uvc_audio_route_cb(void *param)
{
	uvc_audio_worker_signal((struct uvc_audio_worker *)param);
}

static void uvc_audio_error_cb(AAudioStream *stream, void *context, aaudio_result_t error)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(error);
	struct uvc_audio_worker *w = context;
	if (!w)
		return;
	atomic_store_explicit(&w->error_disconnected, true, memory_order_release);
	uvc_audio_worker_signal(w);
}

static bool uvc_audio_wait_deadline(struct timespec *ts, uint32_t timeout_ms)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts) != 0)
		return false;
	ts->tv_sec += timeout_ms / 1000U;
	ts->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
	return true;
}

static AAudioStream *uvc_audio_open(struct uvc_source *s, const char *usb_device, bool first_attempt,
				    struct uvc_audio_worker *worker, const volatile long *stop_flag)
{
	if (s->audio_device == UVC_AUDIO_DISABLED)
		return NULL;
	if (!obs_android_audio_has_permission()) {
		if (first_attempt)
			blog(LOG_WARNING, "uvc: audio input requires Android microphone permission");
		return NULL;
	}
	const bool automatic = s->audio_device == UVC_AUDIO_AUTO;
	const int32_t requested_device = automatic ? obs_android_audio_find_usb_input(usb_device) : s->audio_device;
	if ((automatic && requested_device <= 0) || requested_device < 0) {
		if (automatic && first_attempt)
			blog(LOG_INFO, "uvc: no matching USB audio input for %s yet", usb_device);
		return NULL;
	}

	/* Many USB capture cards expose 16-bit PCM profiles only on Android, so
	 * probe PCM_I16 before PCM_FLOAT and fall back to generic preset/auto channels. */
	const struct {
		aaudio_format_t fmt;
		int32_t channels;
		aaudio_input_preset_t preset;
	} attempts[] = {
		{AAUDIO_FORMAT_PCM_I16, 2, AAUDIO_INPUT_PRESET_UNPROCESSED},
		{AAUDIO_FORMAT_PCM_I16, AAUDIO_UNSPECIFIED, AAUDIO_INPUT_PRESET_UNPROCESSED},
		{AAUDIO_FORMAT_PCM_FLOAT, 2, AAUDIO_INPUT_PRESET_UNPROCESSED},
		{AAUDIO_FORMAT_PCM_FLOAT, AAUDIO_UNSPECIFIED, AAUDIO_INPUT_PRESET_UNPROCESSED},
		{AAUDIO_FORMAT_PCM_I16, AAUDIO_UNSPECIFIED, AAUDIO_INPUT_PRESET_GENERIC},
		{AAUDIO_FORMAT_PCM_FLOAT, AAUDIO_UNSPECIFIED, AAUDIO_INPUT_PRESET_GENERIC},
	};

	AAudioStream *stream = NULL;
	int32_t device = 0;
	int32_t rate = 0;
	int32_t channels = 0;
	aaudio_format_t fmt = AAUDIO_FORMAT_INVALID;
	aaudio_result_t last_result = AAUDIO_OK;

	for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
		if (stop_flag && os_atomic_load_long(stop_flag))
			return NULL;
		AAudioStreamBuilder *builder = NULL;
		last_result = AAudio_createStreamBuilder(&builder);
		if (last_result != AAUDIO_OK || !builder) {
			blog(LOG_WARNING, "uvc: could not create audio stream builder: %d", last_result);
			return NULL;
		}
		AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
		if (requested_device > 0)
			AAudioStreamBuilder_setDeviceId(builder, requested_device);
		AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
		AAudioStreamBuilder_setInputPreset(builder, attempts[i].preset);
		AAudioStreamBuilder_setFormat(builder, attempts[i].fmt);
		if (attempts[i].channels > 0)
			AAudioStreamBuilder_setChannelCount(builder, attempts[i].channels);
		AAudioStreamBuilder_setDataCallback(builder, uvc_audio_data, s);
		if (worker)
			AAudioStreamBuilder_setErrorCallback(builder, uvc_audio_error_cb, worker);

		AAudioStream *candidate = NULL;
		last_result = AAudioStreamBuilder_openStream(builder, &candidate);
		AAudioStreamBuilder_delete(builder);
		if (last_result != AAUDIO_OK || !candidate)
			continue;

		const int32_t got_dev = AAudioStream_getDeviceId(candidate);
		const int32_t got_rate = AAudioStream_getSampleRate(candidate);
		const int32_t got_ch = AAudioStream_getChannelCount(candidate);
		const aaudio_format_t got_fmt = AAudioStream_getFormat(candidate);
		if ((requested_device > 0 && got_dev != requested_device) || got_rate <= 0 || got_ch < 1 ||
		    got_ch > 2 || (got_fmt != AAUDIO_FORMAT_PCM_I16 && got_fmt != AAUDIO_FORMAT_PCM_FLOAT)) {
			if (first_attempt)
				blog(LOG_INFO,
				     "uvc: audio open attempt %zu mismatch (req=%d got=%d rate=%d ch=%d fmt=%d)",
				     i, requested_device, got_dev, got_rate, got_ch, got_fmt);
			AAudioStream_close(candidate);
			continue;
		}

		stream = candidate;
		device = got_dev;
		rate = got_rate;
		channels = got_ch;
		fmt = got_fmt;
		break;
	}

	if (!stream) {
		blog(LOG_WARNING, "uvc: opening audio input %d failed: %d", requested_device, last_result);
		return NULL;
	}

	const aaudio_result_t start_result = AAudioStream_requestStart(stream);
	if (start_result != AAUDIO_OK) {
		blog(LOG_WARNING, "uvc: starting audio input %d failed: %d", device, start_result);
		AAudioStream_close(stream);
		return NULL;
	}
	blog(LOG_INFO, "uvc: audio input started (device=%d rate=%d channels=%d fmt=%d auto_match=%d)", device, rate,
	     channels, fmt, automatic);
	return stream;
}

static void *uvc_audio_worker_main(void *arg)
{
	struct uvc_audio_worker *w = arg;
	struct uvc_source *s = w->s;
	AAudioStream *stream = NULL;
	bool first_attempt = true;

	while (!os_atomic_load_long(&w->stop) && !os_atomic_load_long(&s->shutting_down)) {
		const uint64_t attempt_rev = obs_android_audio_route_revision();
		const bool attempt_perm = obs_android_audio_has_permission();
		atomic_store_explicit(&w->error_disconnected, false, memory_order_release);
		stream = uvc_audio_open(s, w->usb_device, first_attempt, w, &w->stop);
		first_attempt = false;

		if (stream) {
			pthread_mutex_lock(&w->mtx);
			while (!os_atomic_load_long(&w->stop) && !os_atomic_load_long(&s->shutting_down) &&
			       !atomic_exchange_explicit(&w->error_disconnected, false, memory_order_acq_rel) &&
			       AAudioStream_getState(stream) != AAUDIO_STREAM_STATE_DISCONNECTED) {
				struct timespec ts;
				if (uvc_audio_wait_deadline(&ts, 2000))
					pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
				else
					pthread_cond_wait(&w->cond, &w->mtx);
			}
			pthread_mutex_unlock(&w->mtx);

			AAudioStream_close(stream);
			stream = NULL;
			continue;
		}

		/* Do not hammer AAudio periodically when nothing changed; wait for a
		 * device/route change or microphone permission grant before re-probing. */
		pthread_mutex_lock(&w->mtx);
		while (!os_atomic_load_long(&w->stop) && !os_atomic_load_long(&s->shutting_down)) {
			const uint64_t now_rev = obs_android_audio_route_revision();
			const bool now_perm = obs_android_audio_has_permission();
			if (now_rev != attempt_rev || now_perm != attempt_perm)
				break;
			struct timespec ts;
			if (uvc_audio_wait_deadline(&ts, 1000))
				pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
			else
				pthread_cond_wait(&w->cond, &w->mtx);
		}
		pthread_mutex_unlock(&w->mtx);
	}

	return NULL;
}

static void uvc_audio_worker_start(struct uvc_audio_worker *w, struct uvc_source *s, const char *usb_device)
{
	memset(w, 0, sizeof(*w));
	w->s = s;
	if (!usb_device || !*usb_device || s->audio_device == UVC_AUDIO_DISABLED)
		return;
	snprintf(w->usb_device, sizeof(w->usb_device), "%s", usb_device);

	pthread_condattr_t attr;
	bool have_attr = pthread_condattr_init(&attr) == 0;
	bool cond_ok = false;
	if (pthread_mutex_init(&w->mtx, NULL) == 0) {
		if (have_attr && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
			cond_ok = pthread_cond_init(&w->cond, &attr) == 0;
		else
			cond_ok = pthread_cond_init(&w->cond, NULL) == 0;
		if (!cond_ok)
			pthread_mutex_destroy(&w->mtx);
	}
	if (have_attr)
		pthread_condattr_destroy(&attr);
	if (!cond_ok) {
		blog(LOG_WARNING, "uvc: failed to initialize audio worker sync primitives");
		return;
	}
	w->sync_init = true;
	obs_android_audio_add_route_callback(uvc_audio_route_cb, w);
	if (pthread_create(&w->thread, NULL, uvc_audio_worker_main, w) != 0) {
		blog(LOG_WARNING, "uvc: failed to start audio helper thread");
		obs_android_audio_remove_route_callback(uvc_audio_route_cb, w);
		pthread_cond_destroy(&w->cond);
		pthread_mutex_destroy(&w->mtx);
		w->sync_init = false;
		return;
	}
	w->started = true;
}

static void uvc_audio_worker_stop(struct uvc_audio_worker *w)
{
	if (!w || !w->sync_init)
		return;
	os_atomic_set_long(&w->stop, 1);
	uvc_audio_worker_signal(w);
	if (w->started) {
		pthread_join(w->thread, NULL);
		w->started = false;
	}
	obs_android_audio_remove_route_callback(uvc_audio_route_cb, w);
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->mtx);
	w->sync_init = false;
}

static bool usb_device_matches_id(const struct obs_android_usb_device *dev, const char *usb_id)
{
	if (!usb_id || !*usb_id)
		return true;
	const size_t idlen = strlen(usb_id);
	return strncmp(dev->label, usb_id, idlen) == 0 &&
	       (dev->label[idlen] == '\0' || dev->label[idlen] == ' ');
}

/* A reconnected device may have a new USB path, and another device may reuse an
 * old path after reboot/replug. When VID:PID is known, require it on path match
 * too so we never hijack a different camera occupying the old bus address. */
static int find_usb_device(const struct obs_android_usb_device *devs, int count, const char *path,
			   const char *usb_id)
{
	const bool have_id = usb_id && *usb_id;
	if (path && *path) {
		for (int i = 0; i < count; ++i) {
			if (strcmp(devs[i].name, path) == 0 && (!have_id || usb_device_matches_id(&devs[i], usb_id)))
				return i;
		}
	}

	if (have_id) {
		for (int i = 0; i < count; ++i) {
			if (usb_device_matches_id(&devs[i], usb_id))
				return i;
		}
	}
	return -1;
}

enum uvc_deliver_result {
	UVC_DELIVER_CONTINUE = 0,
	UVC_DELIVER_YUYV_FALLBACK,
};

struct uvc_frame_delivery_ctx {
	struct uvc_source *s;
	const struct uvc_dev *d;
	const struct uvc_mode *mode;
	struct uvc_stream *st;
	tjhandle tj;
	uint8_t *bgra;
	size_t bgra_size;
	struct uvc_recovery *recovery;
	uint64_t fps_window_start;
	uint64_t fps_window_frames;
	uint64_t submit_count;
	uint64_t submit_total_ns;
	uint64_t submit_max_ns;
	uint64_t last_frame_ns;
	uint64_t black_win_start;
	unsigned black_checked;
	unsigned black_frames;
	bool yuyv_fallback_done;
	int yuyv_mode_idx;
};

/* Validates an assembled MJPEG or uncompressed frame, pushes it to OBS,
 * updates FPS/recovery counters, and runs the black-screen self-fallback check. */
static enum uvc_deliver_result deliver_assembled_frame(struct uvc_frame_delivery_ctx *ctx, uint32_t flen,
						       uint64_t received_ns)
{
	struct uvc_source *s = ctx->s;
	const struct uvc_dev *d = ctx->d;
	const struct uvc_mode *mode = ctx->mode;
	struct uvc_stream *st = ctx->st;

	st->frames_done++;
	if (st->frames_done <= 10 || st->frames_done % 300 == 0)
		blog(LOG_INFO, "uvc: 帧完成 #%llu %u 字节，首 2 字节=%02X %02X%s",
		     (unsigned long long)st->frames_done, (unsigned)flen,
		     flen > 0 ? (unsigned)st->frame_buf[0] : 0U, flen > 1 ? (unsigned)st->frame_buf[1] : 0U,
		     mode->kind == MODE_MJPEG ? "（MJPEG 应为 FF D8）" : "");

	/* 空帧（stray EOF 产物，flen == 0）：不算坏帧，不碰 bad_streak，直接跳过 */
	if (flen == 0) {
		s->frames_dropped++;
		if (log_allow(&st->ll_badframe))
			blog(LOG_INFO, "uvc: 跳过空帧（孤立 EOF 标记）");
		return UVC_DELIVER_CONTINUE;
	}

	/* 送帧校验：MJPEG 必须 FFD8 开头、FFD9 在尾部填充窗内（找到就截断）；
	 * 未压缩必须够定长（多了截断）。不满足按坏帧丢，Bulk 连续坏帧到上限重同步 */
	if (mode->kind == MODE_MJPEG) {
		const size_t eoi = flen >= 4 && st->frame_buf[0] == 0xFF && st->frame_buf[1] == 0xD8
					   ? find_eoi_tail(st->frame_buf, flen)
					   : 0;
		if (eoi == 0) {
			s->frames_dropped++;
			if (log_allow(&st->ll_badframe))
				blog(LOG_WARNING, "uvc: 丢弃损坏帧（%u 字节，%s）", flen,
				     flen >= 2 && st->frame_buf[0] == 0xFF && st->frame_buf[1] == 0xD8
					     ? "帧尾缺少 FFD9"
					     : "帧头缺少 FFD8");
			if (!d->is_iso && ++st->bad_streak >= BADFRAME_RESYNC) {
				stream_resync(st, "连续损坏帧达到阈值");
				st->bad_streak = 0;
			}
			return UVC_DELIVER_CONTINUE;
		}
		flen = (uint32_t)eoi;
	} else {
		if (flen < st->frame_total) {
			s->frames_dropped++;
			if (log_allow(&st->ll_badframe))
				blog(LOG_WARNING, "uvc: 丢弃不完整帧（%u 字节 < 期望 %u 字节）", flen,
				     (unsigned)st->frame_total);
			if (!d->is_iso && ++st->bad_streak >= BADFRAME_RESYNC) {
				stream_resync(st, "连续坏帧到上限");
				st->bad_streak = 0;
			}
			return UVC_DELIVER_CONTINUE;
		}
		flen = st->frame_total;
	}
	st->bad_streak = 0;
	st->resync_streak = 0;

	const uint64_t submit_started = os_gettime_ns();
	if (mode->kind == MODE_MJPEG) {
		int jw = 0, jh = 0, jsub = 0, jcs = 0;
		if (tjDecompressHeader3(ctx->tj, st->frame_buf, flen, &jw, &jh, &jsub, &jcs) != 0 || jw <= 0 ||
		    jh <= 0) {
			s->frames_dropped++;
			if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
				blog(LOG_WARNING, "uvc: MJPEG 帧头解析失败（%u 字节）：%s", (unsigned)flen,
				     tjGetErrorStr());
			return UVC_DELIVER_CONTINUE;
		}
		if (s->frames_out == 0)
			blog(LOG_INFO, "uvc: JPEG 头 %dx%d subsamp=%s(%d) colorspace=%s(%d)，载荷 %u 字节", jw,
			     jh, tj_subsamp_name(jsub), jsub, tj_cs_name(jcs), jcs, (unsigned)flen);
		const size_t need = (size_t)jw * (size_t)jh * 4;
		if (jw > 8192 || jh > 8192 || need > MAX_FRAME_BYTES * 4) {
			s->frames_dropped++;
			if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
				blog(LOG_WARNING, "uvc: MJPEG 帧头尺寸异常（%dx%d），已丢弃该帧", jw, jh);
			return UVC_DELIVER_CONTINUE;
		}
		if (need > ctx->bgra_size) {
			bfree(ctx->bgra);
			ctx->bgra = bmalloc(need);
			ctx->bgra_size = ctx->bgra ? need : 0;
		}
		if (!ctx->bgra) {
			s->frames_dropped++;
			blog(LOG_WARNING, "uvc: 分配 BGRA 缓冲区失败（%zu 字节），已丢弃该帧", need);
			return UVC_DELIVER_CONTINUE;
		}
		if (tjDecompress2(ctx->tj, st->frame_buf, flen, ctx->bgra, jw, jw * 4, jh, TJPF_BGRA, 0) != 0) {
			s->frames_dropped++;
			if (s->frames_dropped <= 5 || s->frames_dropped % 300 == 0)
				blog(LOG_WARNING, "uvc: MJPEG 解码失败：%s", tjGetErrorStr());
			return UVC_DELIVER_CONTINUE;
		}
		if (s->frames_out == 0)
			blog(LOG_INFO, "uvc: 首帧 JPEG 解码成功（%dx%d -> BGRA %zu 字节）", jw, jh, need);
		const uint32_t prev_w = s->cur_w;
		const uint32_t prev_h = s->cur_h;
		push_frame_bgra(s, ctx->bgra, jw, jh, received_ns);
		s->cur_w = (uint32_t)jw;
		s->cur_h = (uint32_t)jh;
		if (prev_w && prev_h && (prev_w != (uint32_t)jw || prev_h != (uint32_t)jh)) {
			blog(LOG_INFO, "uvc: MJPEG 实际输出分辨率变化 %ux%u -> %dx%d", prev_w, prev_h, jw, jh);
			set_capture_status(s, "Status.Capturing", mode_kind_name(mode->kind), (unsigned)jw,
					   (unsigned)jh, 0);
		}
	} else {
		uvc_cadence_observe_pts(&s->cadence, st->has_pts, st->frame_pts);
		if (!push_frame_raw(s, mode, st, flen, received_ns))
			return UVC_DELIVER_CONTINUE;
		s->cur_w = mode->w;
		s->cur_h = mode->h;
	}

	const uint64_t submit_elapsed = os_gettime_ns() - submit_started;
	ctx->submit_count++;
	ctx->submit_total_ns += submit_elapsed;
	if (submit_elapsed > ctx->submit_max_ns)
		ctx->submit_max_ns = submit_elapsed;
	s->frames_out++;
	ctx->fps_window_frames++;
	ctx->last_frame_ns = os_gettime_ns();
	uvc_recovery_frame(ctx->recovery, ctx->last_frame_ns);

	if (!s->requested_interval && !raw_low_latency(s) && !ctx->yuyv_fallback_done &&
	    (mode->kind == MODE_MJPEG || mode->kind == MODE_NV12)) {
		const uint8_t *fb = mode->kind == MODE_MJPEG ? ctx->bgra : st->frame_buf;
		const size_t fl =
			mode->kind == MODE_MJPEG ? (size_t)s->cur_w * s->cur_h * 4 : (size_t)mode->w * mode->h;
		if (!ctx->black_win_start)
			ctx->black_win_start = ctx->last_frame_ns;
		ctx->black_checked++;
		if (frame_is_black(fb, fl, mode->kind == MODE_MJPEG ? 4 : 1))
			ctx->black_frames++;
		if (ctx->last_frame_ns - ctx->black_win_start >= 3000000000ULL) {
			ctx->yuyv_fallback_done = true;
			if (ctx->black_frames == ctx->black_checked && ctx->black_checked >= 10) {
				int yi = -1;
				for (int i = 0; i < d->n_modes; i++) {
					if (d->modes[i].kind == MODE_YUYV && d->modes[i].w == mode->w &&
					    d->modes[i].h == mode->h) {
						yi = i;
						break;
					}
				}
				if (yi < 0) {
					blog(LOG_WARNING,
					     "uvc: 检测到持续黑屏输出（3 秒内 %u 帧全黑），且设备无同分辨率 YUYV 模式可切换",
					     ctx->black_checked);
				} else {
					blog(LOG_WARNING,
					     "uvc: 检测到持续黑屏输出（3 秒内 %u 帧全黑），自动从 %s 切换为 YUYV 并重新协商",
					     ctx->black_checked, mode_kind_name(mode->kind));
					ctx->yuyv_mode_idx = yi;
					return UVC_DELIVER_YUYV_FALLBACK;
				}
			}
		}
	}

	const uint64_t now = os_gettime_ns();
	if (now - ctx->fps_window_start >= 1000000000ULL) {
		s->cur_fps_x100 =
			(uint32_t)(ctx->fps_window_frames * 100000000000ULL / (now - ctx->fps_window_start));
		ctx->fps_window_start = now;
		ctx->fps_window_frames = 0;
		set_capture_status(s, "Status.CapturingFps", mode_kind_name(mode->kind), (unsigned)s->cur_w,
				   (unsigned)s->cur_h, s->cur_fps_x100 / 100.0);
	}
	if (s->frames_out == 1 || s->frames_out % 300 == 0)
		blog(LOG_INFO, "uvc: 采集第 %llu 帧 %ux%u %s（载荷=%u 字节，累计丢弃=%llu）",
		     (unsigned long long)s->frames_out, (unsigned)s->cur_w, (unsigned)s->cur_h,
		     mode_kind_name(mode->kind), (unsigned)flen, (unsigned long long)s->frames_dropped);
	return UVC_DELIVER_CONTINUE;
}

static enum uvc_mode_kind libuvc_fmt_to_mode_kind(enum uvc_libuvc_fmt fmt)
{
	switch (fmt) {
	case UVC_LIBUVC_FMT_MJPEG:
		return MODE_MJPEG;
	case UVC_LIBUVC_FMT_YUYV:
		return MODE_YUYV;
	case UVC_LIBUVC_FMT_UYVY:
		return MODE_UYVY;
	case UVC_LIBUVC_FMT_NV12:
		return MODE_NV12;
	case UVC_LIBUVC_FMT_P010:
		return MODE_P010;
	case UVC_LIBUVC_FMT_RGB24:
		return MODE_RGB24;
	case UVC_LIBUVC_FMT_BGR24:
		return MODE_BGR24;
	default:
		return MODE_MJPEG;
	}
}

static void convert_libuvc_modes(const struct uvc_libuvc_mode *src, int count, struct uvc_mode *dst)
{
	for (int i = 0; i < count; ++i) {
		memset(&dst[i], 0, sizeof(dst[i]));
		dst[i].kind = libuvc_fmt_to_mode_kind(src[i].fmt);
		dst[i].fmt_index = src[i].fmt_index;
		dst[i].frame_index = src[i].frame_index;
		dst[i].w = src[i].w;
		dst[i].h = src[i].h;
		dst[i].max_frame_size = src[i].max_frame_size;
		dst[i].def_interval = src[i].def_interval;
		dst[i].min_iv = src[i].min_iv;
		dst[i].max_iv = src[i].max_iv;
		dst[i].step_iv = src[i].step_iv;
		dst[i].n_intervals = src[i].n_intervals;
		if (src[i].n_intervals > 0)
			memcpy(dst[i].intervals, src[i].intervals,
			       (size_t)src[i].n_intervals * sizeof(dst[i].intervals[0]));
	}
}

struct uvc_libuvc_retire_job {
	struct uvc_retire_gate *gate;
	struct uvc_libuvc_session *sess;
	int bridge_fd;
};

static void *uvc_libuvc_retire_worker(void *arg)
{
	struct uvc_libuvc_retire_job *job = arg;
	const bool drained = uvc_libuvc_try_drain(job->sess, 5000);
	blog(drained ? LOG_INFO : LOG_WARNING,
	     "uvc[libuvc]: 后台收尾%s，正在释放 libuvc 会话并归还 USB fd=%d",
	     drained ? "已完成全部传输排干" : "排干超时（5s），停止事件线程并回收句柄", job->bridge_fd);
	uvc_libuvc_finish_retire(job->sess);
	if (job->bridge_fd >= 0)
		obs_android_usb_close(job->bridge_fd);
	if (job->gate) {
		pthread_mutex_lock(&job->gate->mtx);
		if (job->gate->active_jobs > 0)
			job->gate->active_jobs--;
		pthread_cond_broadcast(&job->gate->cond);
		pthread_mutex_unlock(&job->gate->mtx);
		uvc_retire_gate_release(job->gate);
	}
	free(job);
	return NULL;
}

static void uvc_retire_libuvc_async(struct uvc_source *s, struct uvc_libuvc_session *sess, int bridge_fd)
{
	struct uvc_libuvc_retire_job *job = calloc(1, sizeof(*job));
	if (!job) {
		uvc_libuvc_finish_retire(sess);
		if (bridge_fd >= 0)
			obs_android_usb_close(bridge_fd);
		return;
	}
	job->sess = sess;
	job->bridge_fd = bridge_fd;
	job->gate = uvc_source_retain_retire_gate(s, true);
	if (job->gate) {
		pthread_mutex_lock(&job->gate->mtx);
		job->gate->active_jobs++;
		pthread_mutex_unlock(&job->gate->mtx);
	}
	pthread_t tid;
	pthread_attr_t attr;
	bool detached_ok = false;
	if (pthread_attr_init(&attr) == 0) {
		if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
		    pthread_create(&tid, &attr, uvc_libuvc_retire_worker, job) == 0)
			detached_ok = true;
		pthread_attr_destroy(&attr);
	}
	if (!detached_ok)
		uvc_libuvc_retire_worker(job);
}

/* Complete UVC capture session powered by the Android-hardened vendored libuvc
 * backend. Reuses the shared mode selection, TurboJPEG decode, OBS video/audio
 * output, and status reporting pipeline. Takes ownership of fd and always
 * returns it via obs_android_usb_close(fd) (synchronously or via async retire). */
static bool uvc_run_once_libuvc(struct uvc_source *s, const char *open_name, int fd,
				char perm_requested_path[OBS_ANDROID_USB_NAME_LEN])
{
	struct uvc_dev d = {.bridge_fd = fd, .chosen = -1};
	struct uvc_stream st = {0};
	struct uvc_frame_delivery_ctx fctx = {.yuyv_mode_idx = -1};
	struct uvc_audio_worker audio_worker = {0};
	struct uvc_libuvc_mode lmodes[MAX_MODES];
	struct uvc_libuvc_session *sess = NULL;
	tjhandle tj = NULL;
	bool modes_changed = false;
	bool progress = false;
	const uint64_t round_out_start = s->frames_out;
	const uint64_t round_drop_start = s->frames_dropped;
	char err_buf[160] = {0};

	int n_lmodes = 0;
	int usb_speed = LIBUSB_SPEED_UNKNOWN;
	int r = uvc_libuvc_open(fd, p010_available, &sess, lmodes, MAX_MODES, &n_lmodes, &usb_speed, err_buf,
				sizeof(err_buf));
	if (r != 0 || !sess) {
		blog(LOG_ERROR, "uvc[libuvc]: 打开设备失败：%s", err_buf[0] ? err_buf : "unknown");
		if (r == LIBUSB_ERROR_ACCESS && open_name && open_name[0] &&
		    strcmp(perm_requested_path, open_name) != 0) {
			const int pr = obs_android_usb_request_permission(open_name);
			blog(LOG_INFO, "uvc[libuvc]: 访问被拒，重新申请 USB 授权（返回 %d）", pr);
			if (pr == 1) {
				snprintf(perm_requested_path, OBS_ANDROID_USB_NAME_LEN, "%s", open_name);
				const char *args[] = {open_name};
				set_status_key(s, "Status.Unauthorized", args, 1);
				s->wait_kind = UVC_WAIT_PERMISSION;
				goto out;
			}
		}
		set_status_int(s, "Status.WrapFdFailed", r);
		goto out;
	}
	d.usb_speed = usb_speed;
	blog(LOG_INFO, "uvc[libuvc]: uvc_wrap 成功（speed=%d，发现 %d 个模式）", usb_speed, n_lmodes);

	if (n_lmodes <= 0) {
		blog(LOG_WARNING, "uvc[libuvc]: 未解析到任何可用视频模式");
		set_status_key(s, "Status.NoModes", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name ? open_name : "");
		goto out;
	}
	if (n_lmodes > MAX_MODES)
		n_lmodes = MAX_MODES;
	convert_libuvc_modes(lmodes, n_lmodes, d.modes);
	d.n_modes = n_lmodes;
	d.eff_fmt = s->prefer_fmt;
	d.eff_w = s->width;
	d.eff_h = s->height;
	d.eff_fps = s->fps;

	{
		struct uvc_mode scanned[MAX_MODES] = {0};
		const int scanned_count = copy_usable_modes(scanned, &d);
		pthread_mutex_lock(&s->mtx);
		if (strcmp(s->mode_device, open_name) != 0 || s->mode_backend != UVC_BACKEND_LIBUVC ||
		    !uvc_modes_equal(s->device_modes, s->device_mode_count, scanned, scanned_count))
			modes_changed = true;
		s->device_mode_count = scanned_count;
		if (scanned_count > 0)
			memcpy(s->device_modes, scanned, (size_t)scanned_count * sizeof(scanned[0]));
		snprintf(s->mode_device, sizeof(s->mode_device), "%s", open_name);
		s->mode_backend = UVC_BACKEND_LIBUVC;
		pthread_mutex_unlock(&s->mtx);
	}
	d.chosen = choose_mode(&d, s);
	if (d.chosen < 0 || d.chosen >= d.n_modes) {
		if (s->requested_interval || is_rgb24(s->prefer_fmt))
			set_status_key(s, "DeviceMode.Unavailable", NULL, 0);
		else if (raw_low_latency(s))
			set_status_key(s, "Status.UnsupportedYuvMode", NULL, 0);
		else
			set_status_key(s, "Status.NoModes", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name ? open_name : "");
		goto out;
	}

	const struct uvc_mode *mode = &d.modes[d.chosen];
	const uint32_t req_interval =
		s->requested_interval ? s->requested_interval : pick_interval(mode, d.eff_fps);
	blog(LOG_INFO, "uvc[libuvc]: 选中模式 %s %ux%u interval=%u（fmt=%u frame=%u）",
	     mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h, (unsigned)req_interval,
	     (unsigned)mode->fmt_index, (unsigned)mode->frame_index);

	uint32_t neg_iv = 0, neg_payload = 0, neg_maxframe = 0;
	r = uvc_libuvc_start(sess, &lmodes[d.chosen], req_interval, &neg_iv, &neg_payload, &neg_maxframe, err_buf,
			     sizeof(err_buf));
	if (r != 0) {
		blog(LOG_ERROR, "uvc[libuvc]: 协商/启流失败：%s", err_buf[0] ? err_buf : "unknown");
		set_status_int(s, "Status.NegotiateFailed", r);
		goto out;
	}
	d.frame_interval = neg_iv ? neg_iv : req_interval;
	d.payload_size = neg_payload;
	d.frame_buf_size = neg_maxframe;

	size_t frame_cap = raw_frame_bytes(mode);
	if (!frame_cap) {
		const size_t min_mjpeg = (size_t)mode->w * (size_t)mode->h * 2;
		frame_cap = neg_maxframe;
		if (frame_cap < mode->max_frame_size)
			frame_cap = mode->max_frame_size;
		if (frame_cap < min_mjpeg)
			frame_cap = min_mjpeg;
		if (frame_cap < 512U * 1024U)
			frame_cap = 512U * 1024U;
		if (frame_cap > MAX_FRAME_BYTES)
			frame_cap = MAX_FRAME_BYTES;
	}
	st.frame_cap = frame_cap;
	st.frame_total = raw_frame_bytes(mode);
	st.frame_buf = bmalloc(st.frame_cap);
	if (mode->kind == MODE_RGB24)
		st.rgba_buf = bmalloc((size_t)mode->w * (size_t)mode->h * 4);
	if (!st.frame_buf || (mode->kind == MODE_RGB24 && !st.rgba_buf)) {
		set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
		goto out;
	}

	if (mode->kind == MODE_MJPEG) {
		tj = tjInitDecompress();
		if (!tj) {
			blog(LOG_ERROR, "uvc[libuvc]: tjInitDecompress 失败：%s", tjGetErrorStr());
			set_status_key(s, "Status.JpegInitFailed", NULL, 0);
			goto out;
		}
	}

	blog(LOG_INFO,
	     "uvc: event=started backend=libuvc format=%s width=%u height=%u negotiated_fps=%.3f usb_speed=%d payload=%u maxframe=%u",
	     mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
	     d.frame_interval ? 10000000.0 / d.frame_interval : 0.0, d.usb_speed, (unsigned)d.payload_size,
	     (unsigned)d.frame_buf_size);

	uvc_audio_worker_start(&audio_worker, s, open_name);
	set_capture_status(s, "Status.Capturing", mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h, 0);
	{
		const bool neg_changed = uvc_set_negotiated_mode(s, open_name, mode, d.frame_interval);
		if (modes_changed || neg_changed) {
			modes_changed = false;
			uvc_notify_properties_updated(s);
		}
	}

	struct uvc_recovery recovery;
	const uint64_t start_ns = os_gettime_ns();
	uvc_recovery_start(&recovery, start_ns);
	fctx.s = s;
	fctx.d = &d;
	fctx.mode = mode;
	fctx.st = &st;
	fctx.tj = tj;
	fctx.recovery = &recovery;
	fctx.fps_window_start = start_ns;

	uint64_t stats_start_ns = start_ns;
	uint64_t stats_frames = s->frames_out;
	uint64_t last_good_ns = start_ns;

	while (!os_atomic_load_long(&s->shutting_down)) {
		const uint8_t *fdata = NULL;
		size_t flen = 0;
		uint32_t fw = 0, fh = 0;
		enum uvc_libuvc_fmt ffmt = UVC_LIBUVC_FMT_MJPEG;
		uint64_t received_ns = 0;

		const int gr = uvc_libuvc_get_frame(sess, &fdata, &flen, &fw, &fh, &ffmt, &received_ns, 200);
		if (gr < 0) {
			blog(LOG_ERROR, "uvc[libuvc]: 采帧失败 (%d)，退出本轮会话", gr);
			set_status_int(s, "Status.PumpError", gr);
			break;
		}
		const uint64_t now_ns = os_gettime_ns();
		if (gr == 1 && fdata && flen > 0) {
			if (flen > st.frame_cap) {
				if (flen <= MAX_FRAME_BYTES) {
					uint8_t *grown = brealloc(st.frame_buf, flen);
					if (grown) {
						st.frame_buf = grown;
						st.frame_cap = flen;
					}
				}
			}
			if (flen <= st.frame_cap) {
				memcpy(st.frame_buf, fdata, flen);
				const uint64_t out_before = s->frames_out;
				const enum uvc_deliver_result dres =
					deliver_assembled_frame(&fctx, (uint32_t)flen, received_ns);
				if (s->frames_out > out_before) {
					last_good_ns = now_ns;
					progress = true;
				}
				if (dres == UVC_DELIVER_YUYV_FALLBACK && fctx.yuyv_mode_idx >= 0 &&
				    fctx.yuyv_mode_idx < d.n_modes) {
					if (uvc_stopping(s))
						break;
					if (uvc_libuvc_stop_stream(sess, 2000) != 0) {
						blog(LOG_WARNING,
						     "uvc[libuvc]: 黑屏切 YUYV 停流超时，移交后台收尾");
						break;
					}
					d.chosen = fctx.yuyv_mode_idx;
					mode = &d.modes[d.chosen];
					fctx.mode = mode;
					st.frame_total = raw_frame_bytes(mode);
					if (st.frame_total > st.frame_cap) {
						uint8_t *grown = brealloc(st.frame_buf, st.frame_total);
						if (!grown) {
							set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
							break;
						}
						st.frame_buf = grown;
						st.frame_cap = st.frame_total;
					}
					const uint32_t yuv_iv = pick_interval(mode, d.eff_fps);
					uint32_t y_iv = 0, y_pay = 0, y_max = 0;
					r = uvc_libuvc_start(sess, &lmodes[d.chosen], yuv_iv, &y_iv, &y_pay, &y_max,
							     err_buf, sizeof(err_buf));
					if (r != 0) {
						blog(LOG_ERROR, "uvc[libuvc]: 黑屏切 YUYV 重新协商失败：%s",
						     err_buf[0] ? err_buf : "unknown");
						break;
					}
					d.frame_interval = y_iv ? y_iv : yuv_iv;
					d.payload_size = y_pay;
					d.frame_buf_size = y_max;
					if (uvc_set_negotiated_mode(s, open_name, mode, d.frame_interval))
						uvc_notify_properties_updated(s);
					last_good_ns = os_gettime_ns();
					continue;
				}
			} else {
				s->frames_dropped++;
			}
		} else if (now_ns - last_good_ns >= 8000000000ULL) {
			blog(LOG_WARNING, "uvc[libuvc]: 连续 8 秒未收到有效帧，退出会话准备重连");
			break;
		}

		if (now_ns - stats_start_ns >= 5000000000ULL) {
			uint64_t err_pkts = 0, bad_pkts = 0;
			uvc_libuvc_stats(sess, &err_pkts, &bad_pkts);
			blog(LOG_INFO,
			     "uvc: event=progress backend=libuvc format=%s width=%u height=%u fps=%.2f "
			     "frames=%llu bad_frames=%llu err_pkts=%llu bad_pkts=%llu",
			     mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
			     (double)(s->frames_out - stats_frames) * 1000000000.0 /
				     (double)(now_ns - stats_start_ns),
			     (unsigned long long)s->frames_out, (unsigned long long)s->frames_dropped,
			     (unsigned long long)err_pkts, (unsigned long long)bad_pkts);
			stats_start_ns = now_ns;
			stats_frames = s->frames_out;
		}
	}

out:
	uvc_audio_worker_stop(&audio_worker);
	if (sess) {
		if (!uvc_libuvc_close_or_keep_for_retire(sess, 2000)) {
			blog(LOG_WARNING,
			     "uvc[libuvc]: 停止传输取消超时，移交后台收尾排干并延迟归还 USB fd=%d", fd);
			uvc_retire_libuvc_async(s, sess, fd);
			sess = NULL;
			fd = -1;
		} else {
			sess = NULL;
		}
	}
	if (tj)
		tjDestroy(tj);
	if (fd >= 0)
		obs_android_usb_close(fd);
	bfree(st.rgba_buf);
	bfree(st.frame_buf);
	bfree(fctx.bgra);
	if (modes_changed)
		uvc_notify_properties_updated(s);
	blog(LOG_INFO, "uvc[libuvc]: 本轮采集结束（本轮送出 %llu 帧，丢弃 %llu 帧；累计 %llu / %llu）",
	     (unsigned long long)(s->frames_out - round_out_start),
	     (unsigned long long)(s->frames_dropped - round_drop_start), (unsigned long long)s->frames_out,
	     (unsigned long long)s->frames_dropped);
	return progress;
}

static bool uvc_run_once(struct uvc_source *s, char perm_requested_path[OBS_ANDROID_USB_NAME_LEN])
{
	struct uvc_dev d = {.bridge_fd = -1};
	struct uvc_stream st = {0};
	struct uvc_raw_payload_state raw = {0};
	struct uvc_bulk_payload_assembler bulk_asm = {0};
	struct uvc_iso_assembler iso_asm = {0};
	struct uvc_frame_delivery_ctx fctx = {.yuyv_mode_idx = -1};
	bool raw_packets = false;
	bool bulk_payload_mode = false;
	unsigned capture_epoch = 1;
	struct obs_android_usb_device devs[8];
	tjhandle tj = NULL;
	struct uvc_audio_worker audio_worker = {0};
	bool streaming = false;
	bool vs_claimed = false;
	bool auto_detach_ok = false;
	bool vc_detached = false;
	bool vs_detached = false;
	bool progress = false;
	bool modes_changed = false;
	struct uvc_active_pump pump = {0};
	uint64_t received_ns = 0, previous_sequence = 0;
	bool have_sequence = false;
	uint64_t parser_gaps = 0, submit_count = 0, submit_total_ns = 0, submit_max_ns = 0;
	uint64_t max_queue_age_ns = 0, stats_start_ns = 0, stats_bytes = 0, stats_urbs = 0;
	uint64_t stats_frames = s->frames_out;
	const uint64_t round_out_start = s->frames_out;
	const uint64_t round_drop_start = s->frames_dropped;
	s->wait_kind = UVC_WAIT_RECOVER;
	s->blocked_device[0] = '\0';
	uvc_ts_smoother_configure(&s->ts_smoother, 0);
	d.chosen = -1;
	d.bulk_raw_fallback = s->bulk_raw_fallback;
	d.bulk_payload_fallback = s->bulk_payload_fallback;

	if (s->backend_switch_settle) {
		/* Wait on the capture thread (where shutting_down == 0) for any in-flight
		 * retire job to return the Android USB fd before opening under the new backend. */
		if (!uvc_retire_wait_idle(s, 3000)) {
			if (!uvc_stopping(s)) {
				blog(LOG_WARNING, "uvc: 切换后端时上一会话仍在后台收尾，稍后重试");
				s->wait_kind = UVC_WAIT_RETIRE;
			}
			return false;
		}
		s->backend_switch_settle = false;
		sleep_check(s, 150);
		if (uvc_stopping(s))
			return false;
	}

	/* ---- 1. 总线清单里找选中的设备 / 授权 ---- */
	if (!s->usb_device[0]) {
		uvc_clear_negotiated_mode(s);
		set_status_key(s, "Status.NoDeviceSelected", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		return false;
	}
	const int ndev = obs_android_usb_enum_devices(devs, 8);
	if (ndev < 0) {
		set_status_key(s, "Status.BusNotReady", NULL, 0);
		s->wait_kind = UVC_WAIT_DEVICE;
		return false;
	}

	const int sel = find_usb_device(devs, ndev, s->usb_device, s->usb_id);
	if (sel < 0) {
		s->bulk_raw_fallback = false;
		s->bulk_payload_fallback = false;
		uvc_clear_negotiated_mode(s);
		const char *args[] = {s->usb_device[0] ? s->usb_device : s->usb_id};
		set_status_key(s, "Status.DeviceMissing", args, 1);
		s->wait_kind = UVC_WAIT_DEVICE;
		return false;
	}
	const char *open_name = devs[sel].name;
	if (strcmp(open_name, s->usb_device) != 0) {
		blog(LOG_INFO, "uvc: 原设备路径 '%s' 不可用，已根据标识 %s 匹配到新路径 '%s'", s->usb_device,
		     s->usb_id, open_name);
		pthread_mutex_lock(&s->mtx);
		snprintf(s->usb_device, sizeof(s->usb_device), "%s", open_name);
		pthread_mutex_unlock(&s->mtx);
		modes_changed = true;
	}

	/* 记下这台设备的 vid:pid（label 第一个空格前的 token），拔插换号或重启后靠它认回来 */
	{
		const char *sp = strchr(devs[sel].label, ' ');
		const size_t idlen = sp ? (size_t)(sp - devs[sel].label) : strlen(devs[sel].label);
		if (idlen > 0 && idlen < sizeof(s->usb_id)) {
			pthread_mutex_lock(&s->mtx);
			snprintf(s->usb_id, sizeof(s->usb_id), "%.*s", (int)idlen, devs[sel].label);
			pthread_mutex_unlock(&s->mtx);
		}
	}

	if (!devs[sel].has_permission) {
		const char *args[] = {open_name};
		set_status_key(s, "Status.Unauthorized", args, 1);
		if (strcmp(perm_requested_path, open_name) != 0) {
			const int pr = obs_android_usb_request_permission(open_name);
			blog(LOG_INFO, "uvc: 设备未授权，已发起授权请求（返回 %d：%s）", pr,
			     pr == 1   ? "弹窗中"
			     : pr == 0 ? "其实已有权限"
				       : "失败");
			snprintf(perm_requested_path, OBS_ANDROID_USB_NAME_LEN, "%s", open_name);
		}
		s->wait_kind = UVC_WAIT_PERMISSION;
		if (modes_changed)
			uvc_notify_properties_updated(s);
		return false;
	}

	if (!uvc_retire_wait_idle(s, 100)) {
		if (!uvc_stopping(s))
			blog(LOG_WARNING, "uvc: 上一会话仍在后台收尾，暂不开启新会话或提交新传输");
		s->wait_kind = UVC_WAIT_RETIRE;
		return false;
	}

	/* ---- 2. 借 fd，包 libusb（nikon-source 真机验证的唯一可行模式） ---- */
	d.bridge_fd = obs_android_usb_open(open_name);
	if (d.bridge_fd < 0) {
		if (strcmp(perm_requested_path, open_name) != 0) {
			const int pr = obs_android_usb_request_permission(open_name);
			blog(LOG_INFO, "uvc: 打开设备失败后尝试重新申请 USB 授权（%s，返回 %d：%s）", open_name, pr,
			     pr == 1   ? "弹窗中"
			     : pr == 0 ? "已有权限"
				       : "失败");
			if (pr == 1) {
				snprintf(perm_requested_path, OBS_ANDROID_USB_NAME_LEN, "%s", open_name);
				const char *args[] = {open_name};
				set_status_key(s, "Status.Unauthorized", args, 1);
				s->wait_kind = UVC_WAIT_PERMISSION;
				return false;
			}
		}
		set_status_int(s, "Status.OpenFailed", d.bridge_fd);
		return false;
	}
	perm_requested_path[0] = '\0';
	set_status_key(s, "Status.Initializing", NULL, 0);

	if (s->backend == UVC_BACKEND_LIBUVC)
		return uvc_run_once_libuvc(s, open_name, d.bridge_fd, perm_requested_path);

	/* 必须关设备发现：默认初始化要建 NETLINK_KOBJECT_UEVENT socket，Android 的 SELinux
	 * 不让应用建，libusb_init 直接 -1（真机实测，旧 libuvc 实现就死在这） */
	const struct libusb_init_option opts[] = {{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}}};
	int r = libusb_init_context(&d.ctx, opts, 1);
	if (r != 0) {
		char rb[16];
		snprintf(rb, sizeof(rb), "%d", r);
		const char *args[] = {rb};
		blog(LOG_ERROR, "uvc: libusb_init_context 失败 %d", r);
		set_status_key(s, "Status.LibusbInitFailed", args, 1);
		goto out;
	}
	r = libusb_wrap_sys_device(d.ctx, (intptr_t)d.bridge_fd, &d.h);
	if (r != 0 || !d.h) {
		char rb[16];
		snprintf(rb, sizeof(rb), "%d", r);
		const char *args[] = {rb};
		blog(LOG_ERROR, "uvc: libusb_wrap_sys_device(fd=%d) 失败 %d", d.bridge_fd, r);
		if (r == LIBUSB_ERROR_ACCESS && open_name && open_name[0] &&
		    strcmp(perm_requested_path, open_name) != 0) {
			const int pr = obs_android_usb_request_permission(open_name);
			blog(LOG_INFO, "uvc: 访问被拒，重新申请 USB 授权（返回 %d）", pr);
			if (pr == 1) {
				snprintf(perm_requested_path, OBS_ANDROID_USB_NAME_LEN, "%s", open_name);
				const char *uargs[] = {open_name};
				set_status_key(s, "Status.Unauthorized", uargs, 1);
				s->wait_kind = UVC_WAIT_PERMISSION;
				d.h = NULL;
				goto out;
			}
		}
		set_status_key(s, "Status.WrapFdFailed", args, 1);
		d.h = NULL;
		goto out;
	}
	{
		/* 链路速率打点：max_packet=1024 已侧面确认 SS，这里拿 libusb 的确证 */
		libusb_device *ld = libusb_get_device(d.h);
		const int sp = ld ? libusb_get_device_speed(ld) : LIBUSB_SPEED_UNKNOWN;
		d.usb_speed = sp;
		blog(LOG_INFO, "uvc: USB 链路速率 speed=%d（%s）", sp,
		     sp == LIBUSB_SPEED_SUPER        ? "SuperSpeed 5Gbps"
		     : sp == LIBUSB_SPEED_SUPER_PLUS ? "SuperSpeed+ 10Gbps"
		     : sp == LIBUSB_SPEED_HIGH       ? "HighSpeed 480 Mbps（非 SuperSpeed）"
		     : sp == LIBUSB_SPEED_FULL       ? "FullSpeed 12Mbps"
		     : sp == LIBUSB_SPEED_LOW        ? "LowSpeed 1.5Mbps"
						     : "未知");
	}

	d.quirks = device_quirks(d.h);
	d.eff_fmt = s->prefer_fmt;
	d.eff_w = s->width;
	d.eff_h = s->height;
	d.eff_fps = s->fps;
	/* 720p60 YUYV is the tested default for this device; 1080p60 can exceed USB bandwidth. */
	if (!s->requested_interval && (d.quirks & QUIRK_PREFER_YUYV) && s->prefer_fmt == MODE_MJPEG &&
	    s->width == 1920 && s->height == 1080 && s->fps == 60) {
		d.eff_fmt = MODE_YUYV;
		d.eff_w = 1280;
		d.eff_h = 720;
		blog(LOG_WARNING, "uvc: 怪癖命中 %s，出厂默认改为 YUYV %dx%d@%d", s->usb_id, d.eff_w,
		     d.eff_h, d.eff_fps);
	}

	/* ---- 3. 描述符扫描 + 模式选择 ---- */
	if (uvc_stopping(s))
		goto out;
	if (!uvc_scan_device(&d)) {
		set_status_key(s, "Status.UnsupportedDevice", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name);
		goto out;
	}
	{
		struct uvc_mode scanned[MAX_MODES] = {0};
		const int scanned_count = copy_usable_modes(scanned, &d);
		pthread_mutex_lock(&s->mtx);
		if (strcmp(s->mode_device, open_name) != 0 || s->mode_backend != UVC_BACKEND_NATIVE ||
		    !uvc_modes_equal(s->device_modes, s->device_mode_count, scanned, scanned_count))
			modes_changed = true;
		s->device_mode_count = scanned_count;
		if (scanned_count > 0)
			memcpy(s->device_modes, scanned, (size_t)scanned_count * sizeof(scanned[0]));
		snprintf(s->mode_device, sizeof(s->mode_device), "%s", open_name);
		s->mode_backend = UVC_BACKEND_NATIVE;
		pthread_mutex_unlock(&s->mtx);
	}
	d.chosen = choose_mode(&d, s);
	if (d.chosen < 0) {
		if (s->requested_interval || is_rgb24(s->prefer_fmt))
			set_status_key(s, "DeviceMode.Unavailable", NULL, 0);
		else if (raw_low_latency(s))
			set_status_key(s, "Status.UnsupportedYuvMode", NULL, 0);
		else
			set_status_key(s, "Status.NoModes", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name);
		goto out;
	}
	const struct uvc_mode *mode = &d.modes[d.chosen];
	if ((raw_low_latency(s) || is_rgb24(mode->kind)) && d.usb_speed > LIBUSB_SPEED_UNKNOWN &&
	    d.usb_speed <= LIBUSB_SPEED_HIGH) {
		const uint32_t iv = s->requested_interval ? s->requested_interval : pick_interval(mode, d.eff_fps);
		if ((uint64_t)raw_frame_bytes(mode) * 10000000ULL > 60000000ULL * iv) {
			set_status_key(s, "Status.UsbTooSlow", NULL, 0);
			s->wait_kind = UVC_WAIT_CONFIG;
			snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name);
			goto out;
		}
	}
	{
		const struct uvc_mode *m = &d.modes[d.chosen];
		blog(LOG_INFO, "uvc: 选中模式 %s fmt=%u frame=%u %ux%u（设置：video_format='%s' %dx%d@%d，kind=%d）",
		     mode_kind_name(m->kind), (unsigned)m->fmt_index, (unsigned)m->frame_index, (unsigned)m->w,
		     (unsigned)m->h, s->fmt_setting, s->width, s->height, s->fps, s->prefer_fmt);
	}

	/* ---- 4. 抢设备：detach 内核 uvcvideo 驱动，再 claim VS 接口 ----
	 * 平板内核会把采集卡绑到 uvcvideo（/dev/videoN），不 detach 的话 claim 撞 -6(BUSY)。
	 * 本机 ROM 实测放行 USBDEVFS_DISCONNECT，但仍按"尽力而为 + 清晰日志"写。 */
	if (uvc_stopping(s))
		goto out;
	r = libusb_set_auto_detach_kernel_driver(d.h, 1);
	auto_detach_ok = (r == 0);
	blog(LOG_INFO, "uvc: set_auto_detach_kernel_driver r=%d%s", r,
	     r == 0 ? "" : "（不支持/失败，claim 时看 libusb 自己怎么处理）");
	if (d.vc_iface >= 0 && d.vc_iface != d.vs_iface) {
		const int kd = libusb_kernel_driver_active(d.h, d.vc_iface);
		const int dr = libusb_detach_kernel_driver(d.h, d.vc_iface);
		vc_detached = (dr == 0);
		blog(LOG_INFO, "uvc: VC 接口 %d 内核驱动=%d，显式解绑 r=%d", d.vc_iface, kd, dr);
	}
	{
		const int kd = libusb_kernel_driver_active(d.h, d.vs_iface);
		const int dr = libusb_detach_kernel_driver(d.h, d.vs_iface);
		vs_detached = (dr == 0);
		blog(LOG_INFO, "uvc: VS 接口 %d 内核驱动=%d，显式解绑 r=%d", d.vs_iface, kd, dr);
	}

	r = libusb_claim_interface(d.h, d.vs_iface);
	if (r != 0) {
		blog(LOG_ERROR,
		     "uvc: claim VS 接口 %d 失败 %d（-6=BUSY：内核 uvcvideo 还占着；"
		     "-5=NOT_FOUND：设备掉了）",
		     d.vs_iface, r);
		set_status_int(s, "Status.ClaimFailed", r);
		goto out;
	}
	progress = true; /* 能 claim 上就算赢了一半（内核驱动 detach 成功） */
	vs_claimed = true;

	/* ---- 4.5 开流前置 SET_INTERFACE（一次性正常开流步骤，不是 ERR 期的反复
	 * 踢脚——那个保持删除）：Windows UVC 驱动每次开流都切到工作 alt，有的固件
	 * 没这一步就只出黑场。ISO 设备在 PROBE/COMMIT 前必须先保持零带宽 alt0 ---- */
	if (uvc_stopping(s))
		goto out;
	if (d.is_iso) {
		const int si = libusb_set_interface_alt_setting(d.h, d.vs_iface, 0);
		blog(LOG_INFO, "uvc: ISO 协商前置切换 alt0 r=%d", si);
	} else {
		uint8_t cur_alt = 0xFF;
		const int gr = libusb_control_transfer(d.h, 0x81, LIBUSB_REQUEST_GET_INTERFACE, 0,
						       (uint16_t)d.vs_iface, &cur_alt, 1, 1000);
		blog(LOG_INFO, "uvc: GET_INTERFACE r=%d，当前 alt=%u（目标 alt=%d）", gr,
		     gr == 1 ? (unsigned)cur_alt : 0xFFU, d.vs_alt);
		if (uvc_stopping(s))
			goto out;
		const int si = libusb_set_interface_alt_setting(d.h, d.vs_iface, d.vs_alt);
		blog(LOG_INFO, "uvc: 设置工作接口替代模式 alt%d r=%d", d.vs_alt, si);
	}

	/* ---- 5 / 5.5 probe/commit 协商 + ISO alt 激活（含首次 URB 提交） ---- */
	if (uvc_stopping(s))
		goto out;
	if (d.is_iso) {
		bool bw_exceeded = false;
		r = uvc_iso_negotiate_and_activate(&d, s, &pump, &bw_exceeded);
		if (r != 0) {
			if (r == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(&pump)) {
				set_status_int(s, "Status.NegotiateFailed", UVC_PUMP_ERR_CANCEL_TIMEOUT);
			} else if (r != LIBUSB_ERROR_INTERRUPTED && !uvc_stopping(s)) {
				if (bw_exceeded) {
					set_status_key(s, "Status.IsoBandwidthExceeded", NULL, 0);
					s->wait_kind = UVC_WAIT_BANDWIDTH;
				} else {
					set_status_int(s, "Status.NegotiateFailed", r);
					if (r == LIBUSB_ERROR_IO || r == LIBUSB_ERROR_TIMEOUT || r == LIBUSB_ERROR_PIPE)
						reset_before_reopen(&d, s, &pump);
				}
			}
			goto out;
		}
	} else {
		r = uvc_negotiate(&d, s, true);
		if (r != 0) {
			if (r != LIBUSB_ERROR_INTERRUPTED && !uvc_stopping(s)) {
				set_status_int(s, "Status.NegotiateFailed", r);
				if (r == LIBUSB_ERROR_IO || r == LIBUSB_ERROR_TIMEOUT || r == LIBUSB_ERROR_PIPE)
					reset_before_reopen(&d, s, &pump);
			}
			goto out;
		}
	}
	mode = &d.modes[d.chosen];

	/* ---- 6. 缓冲 ---- */
	if (uvc_stopping(s))
		goto out;
	size_t frame_cap;
	if (mode->kind == MODE_MJPEG) {
		/* 读回值可能是脏值（这台读回过 16384），取"读回 / 描述符 / w*h(>=1MB)"三者最大，
		 * 避免 2K/4K 高细节 MJPEG 帧在低报 max_frame_size 时溢出 */
		const size_t min_mjpeg =
			(size_t)mode->w * mode->h > 1024U * 1024U ? (size_t)mode->w * mode->h : 1024U * 1024U;
		frame_cap = d.frame_buf_size;
		if (frame_cap < mode->max_frame_size)
			frame_cap = mode->max_frame_size;
		if (frame_cap < min_mjpeg)
			frame_cap = min_mjpeg;
		if (frame_cap > MAX_FRAME_BYTES)
			frame_cap = MAX_FRAME_BYTES;
	} else {
		frame_cap = raw_frame_bytes(mode); /* 未压缩帧长是死的 */
	}
	if (!frame_cap || frame_cap > MAX_FRAME_BYTES) {
		set_status_key(s, "Status.FrameTooLarge", NULL, 0);
		s->wait_kind = UVC_WAIT_CONFIG;
		snprintf(s->blocked_device, sizeof(s->blocked_device), "%s", open_name);
		goto out;
	}
	raw_packets = use_raw_packets(&d, s);
	bulk_payload_mode = use_bulk_payload_assembler(&d, s);
	st.frame_cap = frame_cap;
	st.frame_total = raw_frame_bytes(mode); /* MJPEG = 0 */
	st.frame_buf = bmalloc(st.frame_cap);
	if (mode->kind == MODE_RGB24)
		st.rgba_buf = bmalloc((size_t)mode->w * mode->h * 4);

	if (!d.is_iso && !raw_packets && !bulk_payload_mode)
		ensure_bulk_stream_buffer(&st);

	if (!st.frame_buf || (!d.is_iso && !raw_packets && !bulk_payload_mode && !st.input.data) ||
	    (mode->kind == MODE_RGB24 && !st.rgba_buf)) {
		blog(LOG_ERROR, "uvc: 缓冲分配失败（帧 %zu / 流 %zu 字节）", st.frame_cap, st.input.capacity);
		set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
		goto out;
	}

	if (mode->kind == MODE_MJPEG) {
		tj = tjInitDecompress();
		if (!tj) {
			blog(LOG_ERROR, "uvc: tjInitDecompress 失败：%s", tjGetErrorStr());
			set_status_key(s, "Status.JpegInitFailed", NULL, 0);
			goto out;
		}
	}

	/* Submit/recycle USB buffers independently of parsing and OBS frame copies. */
	if (uvc_stopping(s))
		goto out;
	if (!active_pump_running(&pump)) {
		int pump_error = 0;
		if (!start_active_pump(&d, s, &pump, &pump_error)) {
			set_status_int(s, "Status.PumpStartFailed", pump_error);
			blog(LOG_ERROR, "uvc: %s pump startup failed: %d", d.is_iso ? "iso" : "bulk", pump_error);
			goto out;
		}
	}
	uvc_raw_payload_init(&raw, st.frame_buf, st.frame_total);
	uvc_bulk_payload_init(&bulk_asm, st.frame_buf, st.frame_cap, st.frame_total, mode->kind == MODE_MJPEG,
			      d.payload_size, BULK_CHUNK_SIZE);
	uvc_iso_assembler_init(&iso_asm, st.frame_buf, st.frame_cap, st.frame_total, mode->kind == MODE_MJPEG);
	memset(&s->cadence, 0, sizeof(s->cadence));
	stats_start_ns = os_gettime_ns();
	if (d.is_iso) {
		blog(LOG_INFO,
		     "uvc: event=started transport=iso format=%s width=%u height=%u negotiated_fps=%.3f alt=%d pkt_size=%u pkts_per_urb=%u usb_speed=%d payload=%u",
		     mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
		     d.frame_interval ? 10000000.0 / d.frame_interval : 0.0, d.vs_alt, (unsigned)d.iso_packet_size,
		     pump.iso_pkts_per_xfer, d.usb_speed, d.payload_size);
	} else {
		blog(LOG_INFO,
		     "uvc: event=started format=%s width=%u height=%u negotiated_fps=%.3f raw_packets=%d bulk_payload=%d low_latency=%d usb_speed=%d payload=%u unbuffered=%d",
		     mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
		     d.frame_interval ? 10000000.0 / d.frame_interval : 0.0, raw_packets, bulk_payload_mode,
		     raw_low_latency(s), d.usb_speed, d.payload_size, obs_source_async_unbuffered(s->source));
	}

	streaming = true;
	uvc_audio_worker_start(&audio_worker, s, open_name);
	set_capture_status(s, "Status.Capturing", mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h, 0);
	{
		const uint32_t neg_iv = d.frame_interval ? d.frame_interval : pick_interval(mode, d.eff_fps);
		const bool neg_changed = uvc_set_negotiated_mode(s, open_name, mode, neg_iv);
		if (modes_changed || neg_changed) {
			modes_changed = false;
			uvc_notify_properties_updated(s);
		}
	}
	if (d.is_iso) {
		blog(LOG_INFO, "uvc: 进入 ISO 流循环（端点 0x%02X alt%d，pkt=%u×%u，帧缓冲 %zu 字节）", d.ep_in,
		     d.vs_alt, (unsigned)d.iso_packet_size, pump.iso_pkts_per_xfer, st.frame_cap);
	} else {
		blog(LOG_INFO, "uvc: 进入 bulk 流循环（端点 0x%02X，%d 个读并发，transfer=%u 字节，帧缓冲 %zu 字节）",
		     d.ep_in, XFER_IN_FLIGHT,
		     raw_packets ? uvc_bulk_payload_transfer_size(d.payload_size) : (unsigned)BULK_CHUNK_SIZE,
		     st.frame_cap);
	}

	/* ---- 7. 流循环：ISO 按包重组 / Bulk 攒流同步或按 payload 跨距重组 ---- */
	uint64_t last_wait_log_ns = os_gettime_ns();
	bool first_read_logged = false;
	bool abort_stream = false; /* BMH ERR 超 10 秒：解析循环置位，出循环后拆会话 */
	struct uvc_recovery recovery;
	uvc_recovery_start(&recovery, last_wait_log_ns);
	fctx.s = s;
	fctx.d = &d;
	fctx.mode = mode;
	fctx.st = &st;
	fctx.tj = tj;
	fctx.recovery = &recovery;
	fctx.fps_window_start = last_wait_log_ns;

	while (!os_atomic_load_long(&s->shutting_down)) {
		const int fatal = active_pump_error(&pump);
		if (fatal) {
			struct uvc_pump_view stats;
			active_pump_snapshot(&pump, &stats);
			blog(LOG_ERROR,
			     "uvc: event=pump_fatal error=%d usb_bytes=%llu transfers=%llu errors=%llu pkt_errors=%llu drops=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu",
			     fatal, (unsigned long long)stats.received_bytes, (unsigned long long)stats.transfers,
			     (unsigned long long)stats.errors, (unsigned long long)stats.packet_errors,
			     (unsigned long long)stats.dropped_blocks, (unsigned long long)stats.timeouts,
			     (unsigned long long)stats.stalls, (unsigned long long)stats.overflows,
			     (unsigned long long)stats.io_errors);
			set_status_int(s, "Status.PumpError", fatal);
			if (fatal == LIBUSB_ERROR_IO)
				reset_before_reopen(&d, s, &pump);
			break;
		}

		/* A single deadline covers both no USB data and data that cannot form
		 * valid frames. Never count control/restart time against the next deadline. */
		const enum uvc_recovery_action action = uvc_recovery_poll(&recovery, os_gettime_ns());
		if (action != UVC_RECOVERY_WAIT) {
			int nr = 0;
			if (stop_active_pump(&pump) == UVC_PUMP_ERR_CANCEL_TIMEOUT || active_pump_cancel_timed_out(&pump)) {
				blog(LOG_WARNING, "uvc: 恢复期间 USB 传输取消超时，中止本轮会话并移交后台收尾");
				break;
			}
			if (os_atomic_load_long(&s->shutting_down))
				break;
			if (action == UVC_RECOVERY_RESTART) {
				if (d.is_iso) {
					blog(LOG_WARNING, "uvc: 无有效帧，重置 ISO 接口并重新协商");
					nr = restart_active_pump(&d, s, &pump);
				} else {
					if (raw_packets &&
					    (raw.consec_invalid_headers > 0 || raw.consec_size_errors > 0 ||
					     raw.non_err_bytes_since_good > 0)) {
						const bool persist_fallback = parser_gaps == 0;
						blog(LOG_WARNING,
						     "uvc: 看门狗重启时快速路径仍有异常（连续无效头=%u 长度异常=%u 非ERR字节=%llu "
						     "parser_gaps=%llu），%s回退字节流扫描",
						     raw.consec_invalid_headers, raw.consec_size_errors,
						     (unsigned long long)raw.non_err_bytes_since_good,
						     (unsigned long long)parser_gaps,
						     persist_fallback ? "锁定" : "本轮临时");
						d.bulk_raw_fallback = true;
						if (persist_fallback)
							s->bulk_raw_fallback = true;
					} else if (bulk_payload_mode &&
						   (bulk_asm.consec_invalid_headers > 0 ||
						    bulk_asm.non_err_bytes_since_good > 0) &&
						   bulk_asm.good_frames == 0) {
						const bool persist_fallback = parser_gaps == 0;
						blog(LOG_WARNING,
						     "uvc: 看门狗重启时 Bulk 载荷重组路径未出帧（连续无效头=%u 非ERR字节=%llu "
						     "parser_gaps=%llu），%s回退字节流扫描",
						     bulk_asm.consec_invalid_headers,
						     (unsigned long long)bulk_asm.non_err_bytes_since_good,
						     (unsigned long long)parser_gaps,
						     persist_fallback ? "锁定" : "本轮临时");
						d.bulk_payload_fallback = true;
						if (persist_fallback)
							s->bulk_payload_fallback = true;
					}
					blog(LOG_WARNING, "uvc: 无有效帧，清除端点并重新协商");
					nr = libusb_clear_halt(d.h, d.ep_in);
					if (nr == 0)
						nr = restart_active_pump(&d, s, &pump);
				}
				blog(LOG_WARNING, "uvc: 端点恢复/重新协商结果 %d", nr);
				if (nr == 0) {
					if (d.chosen >= 0 && d.chosen < d.n_modes) {
						mode = &d.modes[d.chosen];
						fctx.mode = mode;
					}
					stream_reset_parse(&st);
					uvc_raw_payload_init(&raw, st.frame_buf, st.frame_total);
					uvc_bulk_payload_init(&bulk_asm, st.frame_buf, st.frame_cap, st.frame_total,
							      mode->kind == MODE_MJPEG, d.payload_size, BULK_CHUNK_SIZE);
					uvc_iso_assembler_init(&iso_asm, st.frame_buf, st.frame_cap, st.frame_total,
							       mode->kind == MODE_MJPEG);
					raw_packets = use_raw_packets(&d, s);
					bulk_payload_mode = use_bulk_payload_assembler(&d, s);
					if (!d.is_iso && !raw_packets && !bulk_payload_mode &&
					    !ensure_bulk_stream_buffer(&st)) {
						set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
						break;
					}
					have_sequence = false;
					stats_bytes = 0;
					stats_urbs = 0;
					stats_frames = s->frames_out;
					stats_start_ns = os_gettime_ns();
					capture_epoch++;
					uvc_recovery_restart_finished(&recovery, os_gettime_ns());
					{
						const uint32_t neg_iv = d.frame_interval ? d.frame_interval
											 : pick_interval(mode, d.eff_fps);
						if (uvc_set_negotiated_mode(s, open_name, mode, neg_iv))
							uvc_notify_properties_updated(s);
					}
					continue;
				}
			}
			/* Only transport failures or a second no-frame deadline justify a
			 * reset; permissions, disconnects and invalid modes do not. */
			if ((action == UVC_RECOVERY_RESET || nr == LIBUSB_ERROR_IO || nr == LIBUSB_ERROR_TIMEOUT ||
			     nr == LIBUSB_ERROR_PIPE) &&
			    nr != UVC_PUMP_ERR_CANCEL_TIMEOUT && !active_pump_cancel_timed_out(&pump) &&
			    !os_atomic_load_long(&s->shutting_down))
				reset_before_reopen(&d, s, &pump);
			break;
		}

		bool need_yuyv_fallback = false;
		if (d.is_iso) {
			struct uvc_iso_block block = {0};
			const int acquired = uvc_iso_pump_acquire(pump.iso, &block, BULK_TIMEOUT_MS);
			if (acquired < 0) {
				const int e = active_pump_error(&pump);
				struct uvc_pump_view stats;
				active_pump_snapshot(&pump, &stats);
				blog(LOG_ERROR,
				     "uvc: event=acquire_stopped error=%d usb_bytes=%llu transfers=%llu errors=%llu pkt_errors=%llu drops=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu",
				     e, (unsigned long long)stats.received_bytes, (unsigned long long)stats.transfers,
				     (unsigned long long)stats.errors, (unsigned long long)stats.packet_errors,
				     (unsigned long long)stats.dropped_blocks, (unsigned long long)stats.timeouts,
				     (unsigned long long)stats.stalls, (unsigned long long)stats.overflows,
				     (unsigned long long)stats.io_errors);
				set_status_int(s, "Status.PumpStopped", e);
				if (e == LIBUSB_ERROR_IO)
					reset_before_reopen(&d, s, &pump);
				break;
			}
			if (!acquired) {
				const uint64_t now = os_gettime_ns();
				if (now - last_wait_log_ns >= 5000000000ULL) {
					struct uvc_pump_view stats;
					active_pump_snapshot(&pump, &stats);
					blog(LOG_INFO,
					     "uvc: event=waiting transport=iso frames=%llu usb_bytes=%llu transfers=%llu errors=%llu pkt_errors=%llu drops=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu",
					     (unsigned long long)s->frames_out,
					     (unsigned long long)stats.received_bytes,
					     (unsigned long long)stats.transfers, (unsigned long long)stats.errors,
					     (unsigned long long)stats.packet_errors,
					     (unsigned long long)stats.dropped_blocks,
					     (unsigned long long)stats.timeouts, (unsigned long long)stats.stalls,
					     (unsigned long long)stats.overflows,
					     (unsigned long long)stats.io_errors);
					last_wait_log_ns = now;
				}
				continue;
			}

			if (!first_read_logged) {
				first_read_logged = true;
				blog(LOG_INFO, "uvc: first ISO URB packets=%u; independent pump with buffer handoff",
				     block.num_packets);
			}
			received_ns = block.received_ns;
			const uint64_t now_ns = os_gettime_ns();
			const uint64_t age = now_ns > received_ns ? now_ns - received_ns : 0;
			if (age > max_queue_age_ns)
				max_queue_age_ns = age;
			if (have_sequence && block.sequence != previous_sequence + 1) {
				parser_gaps++;
				uvc_ts_smoother_mark_discontinuity(&s->ts_smoother);
				if (iso_asm.assembling && !iso_asm.corrupt)
					s->frames_dropped++;
				uvc_iso_assembler_on_gap(&iso_asm);
			}
			have_sequence = true;
			previous_sequence = block.sequence;

			for (unsigned k = 0; k < block.num_packets; k++) {
				const struct uvc_iso_packet_desc *pkt = &block.packets[k];
				if (pkt->status != LIBUSB_TRANSFER_COMPLETED) {
					if (iso_asm.assembling && !iso_asm.corrupt)
						s->frames_dropped++;
					uvc_iso_assembler_on_gap(&iso_asm);
					continue;
				}
				const uint8_t *pdata = block.data + pkt->offset;
				struct uvc_iso_frame ready_frame = {0};
				if (uvc_iso_assembler_check_flush(&iso_asm, pdata, pkt->length, &ready_frame)) {
					st.err_first_ns = 0;
					st.has_pts = ready_frame.have_pts;
					st.frame_pts = ready_frame.pts;
					if (deliver_assembled_frame(&fctx, (uint32_t)ready_frame.len, received_ns) ==
					    UVC_DELIVER_YUYV_FALLBACK) {
						need_yuyv_fallback = true;
						break;
					}
				}
				bool had_bmh_err = false;
				const uint64_t ovf_before = iso_asm.overflow_frames;
				if (uvc_iso_assembler_push(&iso_asm, pdata, pkt->length, &ready_frame,
							   &had_bmh_err)) {
					st.err_first_ns = 0;
					st.has_pts = ready_frame.have_pts;
					st.frame_pts = ready_frame.pts;
					if (deliver_assembled_frame(&fctx, (uint32_t)ready_frame.len, received_ns) ==
					    UVC_DELIVER_YUYV_FALLBACK) {
						need_yuyv_fallback = true;
						break;
					}
				} else if (iso_asm.overflow_frames > ovf_before) {
					s->frames_dropped++;
					if (log_allow(&st.ll_overflow))
						blog(LOG_WARNING, "uvc: ISO 帧超过缓冲（上限 %zu 字节），丢弃本帧",
						     st.frame_cap);
				} else if (had_bmh_err) {
					s->frames_dropped++;
					if (handle_err_header(s, &st)) {
						abort_stream = true;
						break;
					}
				} else if (pkt->length >= 2 && pdata[0] >= 2 && pdata[0] <= pkt->length) {
					st.err_first_ns = 0;
				}
			}
			uvc_iso_pump_release(pump.iso, &block);
			submit_count = fctx.submit_count;
			submit_total_ns = fctx.submit_total_ns;
			submit_max_ns = fctx.submit_max_ns;
		} else {
			struct uvc_bulk_block block = {0};
			const int acquired = uvc_bulk_pump_acquire(pump.bulk, &block, BULK_TIMEOUT_MS);
			if (acquired < 0) {
				const int e = active_pump_error(&pump);
				struct uvc_pump_view stats;
				active_pump_snapshot(&pump, &stats);
				blog(LOG_ERROR,
				     "uvc: event=acquire_stopped error=%d usb_bytes=%llu transfers=%llu errors=%llu drops=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu",
				     e, (unsigned long long)stats.received_bytes, (unsigned long long)stats.transfers,
				     (unsigned long long)stats.errors, (unsigned long long)stats.dropped_blocks,
				     (unsigned long long)stats.timeouts, (unsigned long long)stats.stalls,
				     (unsigned long long)stats.overflows, (unsigned long long)stats.io_errors);
				set_status_int(s, "Status.PumpStopped", e);
				if (e == LIBUSB_ERROR_IO)
					reset_before_reopen(&d, s, &pump);
				break;
			}
			if (!acquired) {
				const uint64_t now = os_gettime_ns();
				if (now - last_wait_log_ns >= 5000000000ULL) {
					struct uvc_pump_view stats;
					active_pump_snapshot(&pump, &stats);
					blog(LOG_INFO,
					     "uvc: event=waiting frames=%llu usb_bytes=%llu transfers=%llu errors=%llu drops=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu",
					     (unsigned long long)s->frames_out,
					     (unsigned long long)stats.received_bytes,
					     (unsigned long long)stats.transfers, (unsigned long long)stats.errors,
					     (unsigned long long)stats.dropped_blocks,
					     (unsigned long long)stats.timeouts, (unsigned long long)stats.stalls,
					     (unsigned long long)stats.overflows,
					     (unsigned long long)stats.io_errors);
					last_wait_log_ns = now;
				}
				continue;
			}

			if (!first_read_logged) {
				first_read_logged = true;
				blog(LOG_INFO, "uvc: first USB block=%zu; independent pump with buffer handoff",
				     block.size);
			}
			received_ns = block.received_ns;
			const uint64_t now_ns = os_gettime_ns();
			const uint64_t age = now_ns > received_ns ? now_ns - received_ns : 0;
			if (age > max_queue_age_ns)
				max_queue_age_ns = age;
			if (have_sequence && block.sequence != previous_sequence + 1) {
				parser_gaps++;
				uvc_ts_smoother_mark_discontinuity(&s->ts_smoother);
				if (raw_packets) {
					const uint64_t before = raw.bad_frames;
					uvc_raw_payload_reset(&raw);
					s->frames_dropped += raw.bad_frames - before;
				} else if (bulk_payload_mode) {
					const uint64_t before = bulk_asm.bad_frames;
					uvc_bulk_payload_on_gap(&bulk_asm);
					s->frames_dropped += bulk_asm.bad_frames - before;
				} else if (st.assembling) {
					s->frames_dropped++;
				}
				stream_reset_parse(&st);
			}
			have_sequence = true;
			previous_sequence = block.sequence;
			if (raw_packets) {
				/* One URB normally carries several fixed-size payloads. Reframe by
				 * dwMaxPayloadTransferSize; a completed frame is delivered before
				 * feeding the rest of the block, so no extra pixel copy is needed. */
				const size_t payload = d.payload_size;
				const uint64_t bad_before = raw.bad_frames;
				bool fell_back = false;
				size_t offset = 0;
				for (; offset + payload <= block.size; offset += payload) {
					if (uvc_raw_payload_feed(&raw, block.data + offset, payload)) {
						st.has_pts = raw.have_pts;
						st.frame_pts = raw.pts;
						if (deliver_assembled_frame(&fctx, (uint32_t)st.frame_total,
									    received_ns) ==
						    UVC_DELIVER_YUYV_FALLBACK) {
							need_yuyv_fallback = true;
							break;
						}
					} else if (uvc_raw_payload_should_fallback(&raw)) {
						fell_back = true;
						break;
					}
				}
				/* UVC allows the last payload of a frame to be shorter than
				 * dwMaxPayloadTransferSize. The short packet still ends a payload,
				 * so feed the remainder instead of treating it as data loss. */
				if (!need_yuyv_fallback && !fell_back && offset < block.size) {
					if (uvc_raw_payload_feed(&raw, block.data + offset, block.size - offset)) {
						st.has_pts = raw.have_pts;
						st.frame_pts = raw.pts;
						if (deliver_assembled_frame(&fctx, (uint32_t)st.frame_total,
									    received_ns) ==
						    UVC_DELIVER_YUYV_FALLBACK)
							need_yuyv_fallback = true;
					} else if (uvc_raw_payload_should_fallback(&raw)) {
						fell_back = true;
					}
				}
				s->frames_dropped += raw.bad_frames - bad_before;
				submit_count = fctx.submit_count;
				submit_total_ns = fctx.submit_total_ns;
				submit_max_ns = fctx.submit_max_ns;
				if (fell_back) {
					const bool persist_fallback = parser_gaps == 0;
					blog(LOG_WARNING,
					     "uvc: Bulk 按载荷(%uB)组帧未匹配设备协议"
					     "（好帧=%llu 连续无效头=%u 累计无效头=%llu 连续长度异常=%u "
					     "累计长度异常=%llu 非ERR未成帧=%lluB 已喂入=%lluB parser_gaps=%llu），"
					     "%s回退字节流扫描",
					     (unsigned)d.payload_size, (unsigned long long)raw.good_frames,
					     raw.consec_invalid_headers, (unsigned long long)raw.invalid_headers,
					     raw.consec_size_errors, (unsigned long long)raw.size_errors,
					     (unsigned long long)raw.non_err_bytes_since_good,
					     (unsigned long long)raw.fed_bytes, (unsigned long long)parser_gaps,
					     persist_fallback ? "自动" : "本轮临时");
					d.bulk_raw_fallback = true;
					if (persist_fallback)
						s->bulk_raw_fallback = true;
					raw_packets = false;
					if (!ensure_bulk_stream_buffer(&st)) {
						uvc_bulk_pump_release(pump.bulk, &block);
						set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
						break;
					}
					stream_reset_parse(&st);
				} else {
					uvc_bulk_pump_release(pump.bulk, &block);
				}
			}
			if (!raw_packets && bulk_payload_mode && !need_yuyv_fallback) {
				const uint64_t bad_before = bulk_asm.bad_frames;
				const uint64_t ovf_before = bulk_asm.overflow_frames;
				struct uvc_bulk_frame ready_frame = {0};
				if (uvc_bulk_payload_check_flush(&bulk_asm, block.data, block.size, &ready_frame)) {
					st.err_first_ns = 0;
					st.has_pts = ready_frame.have_pts;
					st.frame_pts = ready_frame.pts;
					const uint64_t out_before = s->frames_out;
					if (deliver_assembled_frame(&fctx, (uint32_t)ready_frame.len, received_ns) ==
					    UVC_DELIVER_YUYV_FALLBACK)
						need_yuyv_fallback = true;
					if (s->frames_out > out_before)
						uvc_bulk_payload_note_delivered(&bulk_asm);
				}
				bool had_bmh_err = false;
				if (!need_yuyv_fallback) {
					if (uvc_bulk_payload_push(&bulk_asm, block.data, block.size, &ready_frame,
								  &had_bmh_err)) {
						st.err_first_ns = 0;
						st.has_pts = ready_frame.have_pts;
						st.frame_pts = ready_frame.pts;
						const uint64_t out_before = s->frames_out;
						if (deliver_assembled_frame(&fctx, (uint32_t)ready_frame.len,
									    received_ns) ==
						    UVC_DELIVER_YUYV_FALLBACK)
							need_yuyv_fallback = true;
						if (s->frames_out > out_before)
							uvc_bulk_payload_note_delivered(&bulk_asm);
					} else if (bulk_asm.overflow_frames > ovf_before) {
						if (log_allow(&st.ll_overflow))
							blog(LOG_WARNING,
							     "uvc: Bulk 帧超过缓冲（上限 %zu 字节），丢弃本帧",
							     st.frame_cap);
					} else if (had_bmh_err) {
						if (handle_err_header(s, &st))
							abort_stream = true;
					} else if (bulk_asm.consec_invalid_headers == 0) {
						st.err_first_ns = 0;
					}
				}
				s->frames_dropped += bulk_asm.bad_frames - bad_before;
				submit_count = fctx.submit_count;
				submit_total_ns = fctx.submit_total_ns;
				submit_max_ns = fctx.submit_max_ns;
				if (!need_yuyv_fallback && !abort_stream &&
				    uvc_bulk_payload_should_fallback(&bulk_asm)) {
					const bool persist_fallback = parser_gaps == 0;
					blog(LOG_WARNING,
					     "uvc: Bulk 载荷重组(%uB)未匹配设备协议"
					     "（好帧=%llu 连续无效头=%u 累计无效头=%llu 非ERR未成帧=%lluB 已喂入=%lluB "
					     "parser_gaps=%llu），%s回退字节流扫描",
					     (unsigned)d.payload_size, (unsigned long long)bulk_asm.good_frames,
					     bulk_asm.consec_invalid_headers,
					     (unsigned long long)bulk_asm.invalid_headers,
					     (unsigned long long)bulk_asm.non_err_bytes_since_good,
					     (unsigned long long)bulk_asm.fed_bytes, (unsigned long long)parser_gaps,
					     persist_fallback ? "自动" : "本轮临时");
					d.bulk_payload_fallback = true;
					if (persist_fallback)
						s->bulk_payload_fallback = true;
					bulk_payload_mode = false;
					if (!ensure_bulk_stream_buffer(&st)) {
						uvc_bulk_pump_release(pump.bulk, &block);
						set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
						break;
					}
					stream_reset_parse(&st);
				} else {
					uvc_bulk_pump_release(pump.bulk, &block);
				}
			}
			if (!raw_packets && !bulk_payload_mode && !need_yuyv_fallback && !abort_stream) {
				if (!uvc_stream_buffer_append(&st.input, block.data, block.size)) {
					parser_gaps++;
				uvc_ts_smoother_mark_discontinuity(&s->ts_smoother);
					if (st.assembling)
						s->frames_dropped++;
					stream_reset_parse(&st);
					if (!uvc_stream_buffer_append(&st.input, block.data, block.size)) {
						uvc_bulk_pump_release(pump.bulk, &block);
						set_status_key(s, "Status.InputTooLarge", NULL, 0);
						break;
					}
				}
				st.sync_read += block.size;
				uvc_bulk_pump_release(pump.bulk, &block);

				/* 纯字节扫描状态机：payload 不定长，两个头之间的字节全归前一个头。
				 * 帧边界只信 EOF（这台设备用空 payload 带 EOF 位收尾每帧，hex dump 实锤）；
				 * 误同步防护 = 帧内 FID 一致 + PTS 一致（随机数据撞 PTS 约 2^-32） */
				if (st.input.size >= SBUF_HIGH_WATER) {
					if (log_allow(&st.ll_highwater))
						blog(LOG_WARNING,
						     "uvc: 流缓冲区达到高水位（%zu 字节）仍未定位到帧头，丢弃前半部分重新扫描",
						     st.input.size);
					dump_diag(&st);
					sbuf_consume(&st, st.input.size / 2);
					st.scan_off = 0;
					st.assembling = false;
					st.frame_len = 0;
				}

				for (;;) {
					const size_t H = scan_candidate(&st);
					if (H == SIZE_MAX) {
						/* 没有候选：assembling 时这些字节可能是数据（边界未定，先攒着）；
						 * 末尾 1 字节留着——它可能是跨读块的头起点 */
						st.scan_off = st.input.size > 0 ? st.input.size - 1 : 0;
						break;
					}
					const uint8_t hle = st.input.data[H];
					if (H + hle > st.input.size) {
						st.scan_off = H; /* 头可能跨读块，留存待下块拼接 */
						break;
					}

					const uint8_t bmh = st.input.data[H + 1];
					const uint8_t fid = (uint8_t)(bmh & UVC_BMH_FID);
					const bool eof = (bmh & UVC_BMH_EOF) != 0;
					const bool err = (bmh & UVC_BMH_ERR) != 0;
					const bool has_pts_field = (bmh & 0x04) != 0 && hle >= 6;
					const uint32_t pts = has_pts_field ? get_u32le(st.input.data + H + 2) : 0;

					if (!st.hle) {
						/* 首个合法头即同步（没有"找不到就判不兼容"——继续扫就是） */
						st.hle = hle;
						blog(LOG_INFO,
						     "uvc: 同步建立：首个合法头在偏移 %zu（HLE=%u BMH=0x%02X），前面 %zu 字节垃圾已丢",
						     H, (unsigned)hle, (unsigned)bmh, H);
						st.sync_read = 0;
						sbuf_consume(&st, H);
						st.scan_off = 0;
						continue;
					}

					if (!st.assembling) {
						/* 期待帧头：EOF 头是上一帧的收尾标记（重同步后常见），跳过；
						 * 其余合法候选开新帧。帧间隙的字节是垃圾，随前缀一起丢 */
						sbuf_consume(&st, H + hle);
						st.scan_off = 0;
						if (eof)
							continue;
						if (err) {
							s->frames_dropped++;
							if (handle_err_header(s, &st)) {
								abort_stream = true;
								break;
							}
							continue;
						}
						st.assembling = true;
						st.frame_len = 0;
						st.last_fid = fid;
						st.has_pts = has_pts_field;
						st.frame_pts = pts;
						st.rejected = 0;
						st.err_first_ns = 0; /* 正常头来了：本段 ERR 算过去了 */
						continue;
					}

					/* 帧内关系：FID 一致 且（双方都有 PTS 时）PTS 相等 = 同帧 payload 边界。
					 * FID 翻 / PTS 变 = "边界声称"——帧边界只信 EOF，拒；连续拒到上限
					 * 说明 EOF 真丢了（丢包），重同步 */
					const bool same_frame = fid == st.last_fid &&
								(!has_pts_field || !st.has_pts || pts == st.frame_pts);
					if (!same_frame) {
						if (++st.rejected >= REJECT_RESYNC) {
							stream_resync(&st, "边界声称连拒到上限，EOF 应该丢了");
							continue;
						}
						st.scan_off = H + 1; /* 伪头，这些字节是数据，继续往后扫 */
						continue;
					}
					st.rejected = 0;
					/* Pixel bytes can resemble an ERR header. Validate frame identity
					 * first, so a false header cannot discard arbitrary raw frames. */
					if (err) {
						s->frames_dropped++;
						st.assembling = false;
						st.frame_len = 0;
						sbuf_consume(&st, H + hle);
						st.scan_off = 0;
						if (handle_err_header(s, &st)) {
							abort_stream = true;
							break;
						}
						continue;
					}
					st.err_first_ns = 0; /* 同帧正常头：本段 ERR 算过去了 */
					if (has_pts_field && !st.has_pts) {
						st.has_pts = true;
						st.frame_pts = pts;
					}

					/* H 前的字节全是本帧数据 */
					if (st.frame_len + H > st.frame_cap) {
						if (log_allow(&st.ll_overflow))
							blog(LOG_WARNING,
							     "uvc: 帧超过缓冲（%zu + %zu > %zu），丢弃本帧",
							     st.frame_len, H, st.frame_cap);
						s->frames_dropped++;
						st.assembling = false;
						st.frame_len = 0;
						sbuf_consume(&st, H + hle);
						st.scan_off = 0;
						continue;
					}
					/* MJPEG 帧首早检：首块数据不以 FFD8 开头 = 假帧头/丢了帧首，
					 * 立刻回期待态（比攒到帧尾再丢省得多） */
					if (st.frame_len == 0 && H >= 2 && mode->kind == MODE_MJPEG &&
					    (st.input.data[0] != 0xFF || st.input.data[1] != 0xD8)) {
						st.assembling = false;
						sbuf_consume(&st, H + hle);
						st.scan_off = 0;
						continue;
					}
					memcpy(st.frame_buf + st.frame_len, st.input.data, H);
					st.frame_len += H;
					sbuf_consume(&st, H + hle);
					st.scan_off = 0;

					if (!eof)
						continue;

					/* ---- 一帧齐了（EOF：无论这个 payload 带不带数据，0 字节也合法） ---- */
					st.assembling = false;
					const uint32_t flen = (uint32_t)st.frame_len;
					st.frame_len = 0;

					if (deliver_assembled_frame(&fctx, flen, received_ns) ==
					    UVC_DELIVER_YUYV_FALLBACK) {
						need_yuyv_fallback = true;
						break;
					}
				}
				submit_count = fctx.submit_count;
				submit_total_ns = fctx.submit_total_ns;
				submit_max_ns = fctx.submit_max_ns;
			}
		}

		if (need_yuyv_fallback && fctx.yuyv_mode_idx >= 0) {
			if (uvc_stopping(s))
				break;
			d.chosen = fctx.yuyv_mode_idx;
			mode = &d.modes[d.chosen];
			fctx.mode = mode;
			st.frame_total = raw_frame_bytes(mode);
			if (st.frame_total > st.frame_cap) {
				bfree(st.frame_buf);
				st.frame_cap = st.frame_total;
				st.frame_buf = bmalloc(st.frame_cap);
			}
			if (!st.frame_buf) {
				blog(LOG_ERROR, "uvc: 切 YUYV 重配帧缓冲失败（%zu 字节）", st.frame_cap);
				abort_stream = true;
				break;
			}
			d.iso_override_interval = 0;
			const int nr = restart_active_pump(&d, s, &pump);
			blog(LOG_WARNING, "uvc: 黑场自降级的重新协商结果 %d，解析状态已重置", nr);
			stream_reset_parse(&st);
			uvc_raw_payload_init(&raw, st.frame_buf, st.frame_total);
			uvc_bulk_payload_init(&bulk_asm, st.frame_buf, st.frame_cap, st.frame_total, false,
					      d.payload_size, BULK_CHUNK_SIZE);
			uvc_iso_assembler_init(&iso_asm, st.frame_buf, st.frame_cap, st.frame_total, false);
			raw_packets = use_raw_packets(&d, s);
			bulk_payload_mode = use_bulk_payload_assembler(&d, s);
			have_sequence = false;
			stats_bytes = 0;
			stats_urbs = 0;
			stats_frames = s->frames_out;
			stats_start_ns = os_gettime_ns();
			capture_epoch++;
			if (nr != 0) {
				abort_stream = true;
				break;
			}
			{
				const struct uvc_mode *cur_mode =
					(d.chosen >= 0 && d.chosen < d.n_modes) ? &d.modes[d.chosen] : mode;
				const uint32_t neg_iv =
					d.frame_interval ? d.frame_interval : pick_interval(cur_mode, d.eff_fps);
				if (uvc_set_negotiated_mode(s, open_name, cur_mode, neg_iv))
					uvc_notify_properties_updated(s);
			}
			if (!d.is_iso && !raw_packets && !bulk_payload_mode && !ensure_bulk_stream_buffer(&st)) {
				blog(LOG_ERROR, "uvc: 切 YUYV 分配流缓冲失败");
				set_status_key(s, "Status.OutOfMemoryBuffer", NULL, 0);
				abort_stream = true;
				break;
			}
			uvc_recovery_start(&recovery, os_gettime_ns());
			continue;
		}

		if (abort_stream || !active_pump_running(&pump))
			break;
		const uint64_t stats_now = os_gettime_ns();
		if (stats_now - stats_start_ns >= 5000000000ULL) {
			struct uvc_pump_view stats;
			active_pump_snapshot(&pump, &stats);
			if (d.is_iso) {
				blog(LOG_INFO,
				     "uvc: event=progress transport=iso epoch=%u format=%s width=%u height=%u fps=%.2f usb_MBps=%.2f "
				     "frames=%llu bad_frames=%llu queue_drops=%llu usb_errors=%llu pkt_errors=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu parser_gaps=%llu "
				     "ready_peak=%u ready_capacity=%u active_xfers=%u urbs_per_s=%llu submit_avg_us=%llu submit_max_us=%llu queue_age_max_us=%llu",
				     capture_epoch, mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
				     (double)(s->frames_out - stats_frames) * 1000000000.0 /
					     (double)(stats_now - stats_start_ns),
				     (double)(stats.received_bytes - stats_bytes) * 1000.0 /
					     (double)(stats_now - stats_start_ns),
				     (unsigned long long)s->frames_out, (unsigned long long)s->frames_dropped,
				     (unsigned long long)stats.dropped_blocks, (unsigned long long)stats.errors,
				     (unsigned long long)stats.packet_errors, (unsigned long long)stats.timeouts,
				     (unsigned long long)stats.stalls, (unsigned long long)stats.overflows,
				     (unsigned long long)stats.io_errors, (unsigned long long)parser_gaps,
				     stats.ready_high_water, stats.ready_capacity, stats.active_xfers,
				     (unsigned long long)((stats.transfers - stats_urbs) * 1000000000ULL /
							  (stats_now - stats_start_ns)),
				     (unsigned long long)(submit_count ? submit_total_ns / submit_count / 1000 : 0),
				     (unsigned long long)(submit_max_ns / 1000),
				     (unsigned long long)(max_queue_age_ns / 1000));
			} else {
				blog(LOG_INFO,
				     "uvc: event=progress epoch=%u format=%s width=%u height=%u fps=%.2f usb_MBps=%.2f "
				     "frames=%llu bad_frames=%llu queue_drops=%llu usb_errors=%llu timeouts=%llu stalls=%llu overflows=%llu io_errors=%llu parser_gaps=%llu "
				     "ready_peak=%u ready_capacity=%u compacted_bytes=%llu urbs_per_s=%llu submit_avg_us=%llu submit_max_us=%llu queue_age_max_us=%llu",
				     capture_epoch, mode_kind_name(mode->kind), (unsigned)mode->w, (unsigned)mode->h,
				     (double)(s->frames_out - stats_frames) * 1000000000.0 /
					     (double)(stats_now - stats_start_ns),
				     (double)(stats.received_bytes - stats_bytes) * 1000.0 /
					     (double)(stats_now - stats_start_ns),
				     (unsigned long long)s->frames_out, (unsigned long long)s->frames_dropped,
				     (unsigned long long)stats.dropped_blocks, (unsigned long long)stats.errors,
				     (unsigned long long)stats.timeouts, (unsigned long long)stats.stalls,
				     (unsigned long long)stats.overflows, (unsigned long long)stats.io_errors,
				     (unsigned long long)parser_gaps, stats.ready_high_water, stats.ready_capacity,
				     (unsigned long long)st.input.compacted_bytes,
				     (unsigned long long)((stats.transfers - stats_urbs) * 1000000000ULL /
							  (stats_now - stats_start_ns)),
				     (unsigned long long)(submit_count ? submit_total_ns / submit_count / 1000 : 0),
				     (unsigned long long)(submit_max_ns / 1000),
				     (unsigned long long)(max_queue_age_ns / 1000));
			}
			if (mode->kind != MODE_MJPEG) {
				struct uvc_cadence_stats *c = &s->cadence;
				blog(LOG_INFO, "uvc: event=cadence_input samples=%llu same_luma_sample=%llu "
				     "dt_lt8ms=%llu dt_8_14ms=%llu dt_14_19ms=%llu dt_19_25ms=%llu dt_ge25ms=%llu dt_max_us=%llu",
				     (unsigned long long)c->samples, (unsigned long long)c->same_hash,
				     (unsigned long long)c->dt[0], (unsigned long long)c->dt[1],
				     (unsigned long long)c->dt[2], (unsigned long long)c->dt[3],
				     (unsigned long long)c->dt[4], (unsigned long long)(c->max_dt / 1000));
				blog(LOG_INFO, "uvc: event=cadence_pts frames=%llu pairs=%llu same=%llu backwards=%llu delta_min=%u delta_max=%u",
				     (unsigned long long)c->pts_frames, (unsigned long long)c->pts_pairs,
				     (unsigned long long)c->pts_same, (unsigned long long)c->pts_wraps,
				     (unsigned)c->pts_delta_min, (unsigned)c->pts_delta_max);
				uvc_cadence_reset_window(c);
			}
			{
				struct uvc_ts_smoother *sm = &s->ts_smoother;
				blog(LOG_INFO,
				     "uvc: event=ts_smoother enabled=%d period_us=%llu total=%llu smoothed=%llu "
				     "realign_disc=%llu realign_gap=%llu realign_drift=%llu clamps=%llu max_err_us=%lld",
				     !obs_source_async_unbuffered(s->source) ? 1 : 0,
				     (unsigned long long)(sm->period_ns / 1000ULL),
				     (unsigned long long)sm->total_frames, (unsigned long long)sm->smoothed_frames,
				     (unsigned long long)sm->realign_discontinuity,
				     (unsigned long long)sm->realign_gap, (unsigned long long)sm->realign_drift,
				     (unsigned long long)sm->monotonic_clamps,
				     (long long)(sm->max_abs_err_ns / 1000LL));
				uvc_ts_smoother_reset_window(sm);
			}
			stats_start_ns = stats_now;
			stats_bytes = stats.received_bytes;
			stats_urbs = stats.transfers;
			stats_frames = s->frames_out;
		}
		if (abort_stream)
			break; /* BMH ERR 超 10 秒：拆会话走重开链 */
	}

out:
	uvc_audio_worker_stop(&audio_worker);
	/* All callbacks must finish before closing the device/context or freeing parser buffers. */
	stop_active_pump(&pump);
	if (tj)
		tjDestroy(tj);
	if (active_pump_cancel_timed_out(&pump)) {
		blog(LOG_WARNING, "uvc: USB 传输取消超时，移交后台收尾回收传输、缓冲、句柄与上下文");
		uvc_retire_session_async(s, pump.retained_bulk, pump.retained_iso, d.ctx, d.h, d.bridge_fd, vs_claimed,
					 d.is_iso, d.vs_iface, d.vc_iface, vc_detached, vs_detached, auto_detach_ok);
		pump.retained_bulk = NULL;
		pump.retained_iso = NULL;
		d.ctx = NULL;
		d.h = NULL;
		d.bridge_fd = -1;
	} else {
		if (d.h) {
			if (streaming)
				blog(LOG_INFO, "uvc: 已退出 %s 数据流循环", d.is_iso ? "ISO" : "bulk");
			uvc_release_and_restore_interfaces(d.h, vs_claimed, d.is_iso, d.vs_iface, d.vc_iface,
							   vc_detached, vs_detached, auto_detach_ok);
			libusb_close(d.h); /* auto_detach 开着时 VS 内核驱动会在 release 时重绑 */
		}
		if (d.ctx)
			libusb_exit(d.ctx);
		if (d.bridge_fd >= 0)
			obs_android_usb_close(d.bridge_fd); /* fd 归 Java，只还不关 */
	}
	bfree(st.input.storage);
	bfree(st.rgba_buf);
	if (st.frame_buf)
		bfree(st.frame_buf);
	if (fctx.bgra)
		bfree(fctx.bgra);
	if (modes_changed)
		uvc_notify_properties_updated(s);

	blog(LOG_INFO, "uvc: 本轮采集结束（本轮送出 %llu 帧，丢弃 %llu 帧；累计 %llu / %llu）",
	     (unsigned long long)(s->frames_out - round_out_start),
	     (unsigned long long)(s->frames_dropped - round_drop_start), (unsigned long long)s->frames_out,
	     (unsigned long long)s->frames_dropped);
	return progress;
}

static bool uvc_blocked_device_unchanged(const struct uvc_source *s)
{
	if (!s->blocked_device[0])
		return true; /* NoDeviceSelected: stays parked until settings change */
	struct obs_android_usb_device devs[8];
	const int ndev = obs_android_usb_enum_devices(devs, 8);
	if (ndev <= 0)
		return false;
	for (int i = 0; i < ndev; ++i) {
		if (strcmp(devs[i].name, s->blocked_device) == 0)
			return true;
	}
	return false;
}

static uint64_t uvc_usb_topology_hash(void)
{
	struct obs_android_usb_device devs[8];
	const int ndev = obs_android_usb_enum_devices(devs, 8);
	uint64_t h = 1469598103934665603ULL ^ (uint64_t)(ndev + 1);
	for (int i = 0; i < ndev; ++i) {
		for (const char *p = devs[i].name; *p; ++p)
			h = (h ^ (uint8_t)*p) * 1099511628211ULL;
		h = (h ^ 0xFFU) * 1099511628211ULL;
		for (const char *p = devs[i].label; *p; ++p)
			h = (h ^ (uint8_t)*p) * 1099511628211ULL;
	}
	return h;
}

static void *uvc_thread_main(void *arg)
{
	struct uvc_source *s = arg;
	uint64_t retry_ms = 0;
	unsigned dead_rounds = 0;
	char perm_requested_path[OBS_ANDROID_USB_NAME_LEN] = {0};
	char last_logged_wait[sizeof(s->status)] = {0};

	blog(LOG_INFO, "uvc: 采集线程启动（设备 '%s'，标识 '%s'，设置 video_format='%s' %dx%d@%d）", s->usb_device,
	     s->usb_id, s->fmt_setting, s->width, s->height, s->fps);

	while (!os_atomic_load_long(&s->shutting_down)) {
		/* 死活按"本轮有没有送出帧"判，不能按 progress：猝死的实例 claim/SET 仍通
		 * （progress=true），但 GET/bulk 全 -1、永远送不出帧（真机 1617 实录） */
		const uint64_t out_before = s->frames_out;
		const bool progress = uvc_run_once(s, perm_requested_path);
		if (os_atomic_load_long(&s->shutting_down))
			break;

		char prev[sizeof(s->status)];
		pthread_mutex_lock(&s->mtx);
		snprintf(prev, sizeof(prev), "%s", s->status);
		pthread_mutex_unlock(&s->mtx);

		if (s->wait_kind == UVC_WAIT_CONFIG) {
			/* Configuration/capability mismatch or no device selected:
			 * do NOT retry in a loop or overwrite status with "Unresponsive".
			 * Park cleanly until settings change or the USB device changes. */
			dead_rounds = 0;
			retry_ms = 0;
			blog(LOG_INFO, "uvc: 暂停自动重连，等待修改配置或更换设备（当前状态：%s）", prev);
			while (!os_atomic_load_long(&s->shutting_down)) {
				sleep_check(s, 2000);
				if (os_atomic_load_long(&s->shutting_down))
					break;
				if (!uvc_blocked_device_unchanged(s)) {
					blog(LOG_INFO, "uvc: 检测到 USB 设备变化，重新检测设备能力");
					break;
				}
			}
			last_logged_wait[0] = '\0';
			continue;
		}
		if (s->wait_kind == UVC_WAIT_BANDWIDTH) {
			/* USB ISO bandwidth contention is transient (another USB device may be
			 * unplugged or another stream stopped). Keep Status.IsoBandwidthExceeded
			 * visible, wake immediately on any USB topology change, and otherwise
			 * retry gently every 5 seconds without tripping Status.Unresponsive. */
			dead_rounds = 0;
			retry_ms = 0;
			if (strcmp(last_logged_wait, prev) != 0) {
				blog(LOG_INFO, "uvc: 等待 USB 等时带宽释放（%s）", prev);
				snprintf(last_logged_wait, sizeof(last_logged_wait), "%s", prev);
			}
			const uint64_t topo0 = uvc_usb_topology_hash();
			for (int step = 0; step < 5 && !os_atomic_load_long(&s->shutting_down); ++step) {
				sleep_check(s, 1000);
				if (os_atomic_load_long(&s->shutting_down))
					break;
				if (uvc_usb_topology_hash() != topo0) {
					blog(LOG_INFO, "uvc: 检测到 USB 总线设备变化，立即重试带宽分配");
					break;
				}
			}
			continue;
		}
		if (s->wait_kind == UVC_WAIT_PERMISSION) {
			retry_ms = PERM_POLL_MS;
			if (strcmp(last_logged_wait, prev) != 0) {
				blog(LOG_INFO, "uvc: 等待 USB 授权（%s）", prev);
				snprintf(last_logged_wait, sizeof(last_logged_wait), "%s", prev);
			}
			sleep_check(s, (uint32_t)retry_ms);
			continue;
		}
		if (s->wait_kind == UVC_WAIT_DEVICE) {
			dead_rounds = 0;
			retry_ms = 2000;
			if (strcmp(last_logged_wait, prev) != 0) {
				blog(LOG_INFO, "uvc: 等待 USB 设备接入（%s）", prev);
				snprintf(last_logged_wait, sizeof(last_logged_wait), "%s", prev);
			}
			sleep_check(s, (uint32_t)retry_ms);
			continue;
		}
		if (s->wait_kind == UVC_WAIT_RETIRE) {
			retry_ms = 250;
			sleep_check(s, (uint32_t)retry_ms);
			continue;
		}

		last_logged_wait[0] = '\0';
		if (s->frames_out > out_before) {
			dead_rounds = 0;
			retry_ms = RETRY_BACKOFF_MIN_MS;
		} else if (++dead_rounds >= DEAD_ROUND_LIMIT) {
			retry_ms = DEAD_ROUND_BACKOFF_MS;
			char rounds[16];
			snprintf(rounds, sizeof(rounds), "%u", dead_rounds);
			const char *args[] = {rounds};
			set_status_key(s, "Status.Unresponsive", args, 1);
			blog(LOG_WARNING, "uvc: 连续 %u 轮送不出帧，自动重连退避 %llu ms",
			     dead_rounds, (unsigned long long)retry_ms);
		} else {
			/* 本轮有进展（claim 上了接口）就重置退避：真断流/拔线才让等待指数拉长 */
			retry_ms = progress ? RETRY_BACKOFF_MIN_MS
				   : retry_ms
					   ? (retry_ms * 2 > RETRY_BACKOFF_MAX_MS ? RETRY_BACKOFF_MAX_MS : retry_ms * 2)
					   : RETRY_BACKOFF_MIN_MS;
		}
		pthread_mutex_lock(&s->mtx);
		snprintf(prev, sizeof(prev), "%s", s->status);
		pthread_mutex_unlock(&s->mtx);
		blog(LOG_INFO, "uvc: %llu ms 后重试（上次状态：%s）", (unsigned long long)retry_ms, prev);
		sleep_check(s, (uint32_t)retry_ms);
	}

	blog(LOG_INFO, "uvc: 采集线程退出");
	return NULL;
}

/* ---------------- 线程启停（照 nikon-source） ---------------- */

static void uvc_start_thread(struct uvc_source *s)
{
	if (os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 0);
	if (pthread_create(&s->thread, NULL, uvc_thread_main, s) != 0) {
		blog(LOG_ERROR, "uvc: 采集线程创建失败");
		return;
	}
	os_atomic_set_long(&s->thread_started, 1);
}

static void uvc_stop_thread(struct uvc_source *s)
{
	if (!os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->shutting_down, 1);
	uvc_retire_gate_signal(s);
	/* join 最坏堵一个 bulk 超时（1 s）。落在 deactivate/destroy（UI/视频线程）上，
	 * 与 nikon-source 里"join 最坏堵一个 PTP 超时"是同一类取舍。 */
	pthread_join(s->thread, NULL);
	os_atomic_set_long(&s->thread_started, 0);
}

/* ---------------- 源接口 ---------------- */

static const char *uvc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("UvcInput");
}

static void uvc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "uvc_backend", "native");
	obs_data_set_default_string(settings, "device_mode", "");
	obs_data_set_default_int(settings, "yuv_colorspace", VIDEO_CS_709);
	obs_data_set_default_int(settings, "color_range", VIDEO_RANGE_DEFAULT);
	obs_data_set_default_int(settings, "audio_device", UVC_AUDIO_AUTO);
	obs_data_set_default_string(settings, "usb_device", "");
	obs_data_set_default_string(settings, "usb_device_id", "");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "fps", 60);
	obs_data_set_default_string(settings, "video_format", "mjpeg");
	obs_data_set_default_bool(settings, "yuv_low_latency", true);
	obs_data_set_default_bool(settings, "low_latency_unbuffered", false);
}

static enum uvc_backend_kind backend_string_to_kind(const char *name)
{
	if (name && strcmp(name, "libuvc") == 0)
		return UVC_BACKEND_LIBUVC;
	return UVC_BACKEND_NATIVE;
}

static int fmt_string_to_kind(const char *fmt)
{
	if (fmt && strcmp(fmt, "mjpeg") == 0)
		return MODE_MJPEG;
	if (fmt && strcmp(fmt, "yuyv") == 0)
		return MODE_YUYV;
	if (fmt && strcmp(fmt, "uyvy") == 0)
		return MODE_UYVY;
	if (fmt && strcmp(fmt, "nv12") == 0)
		return MODE_NV12;
	if (fmt && strcmp(fmt, "p010") == 0)
		return MODE_P010;
	if (fmt && strcmp(fmt, "rgb24") == 0)
		return MODE_RGB24;
	if (fmt && strcmp(fmt, "bgr24") == 0)
		return MODE_BGR24;
	return -1; /* 自动 */
}

static bool uvc_resolve_unbuffered(obs_data_t *settings)
{
	if (!obs_data_has_user_value(settings, "low_latency_unbuffered") &&
	    obs_data_has_user_value(settings, "yuv_unbuffered")) {
		const bool legacy = obs_data_get_bool(settings, "yuv_unbuffered");
		obs_data_set_bool(settings, "low_latency_unbuffered", legacy);
		return legacy;
	}
	return obs_data_get_bool(settings, "low_latency_unbuffered");
}

static void uvc_apply_async_buffering(struct uvc_source *s, bool unbuffered)
{
	if (obs_source_async_unbuffered(s->source) != unbuffered)
		obs_source_set_async_unbuffered(s->source, unbuffered);
	if (obs_source_async_decoupled(s->source) != unbuffered)
		obs_source_set_async_decoupled(s->source, unbuffered);
}

static void uvc_settings_apply(struct uvc_source *s, obs_data_t *settings)
{
	const char *dev = obs_data_get_string(settings, "usb_device");
	const char *saved_id = obs_data_get_string(settings, "usb_device_id");
	const bool dev_cleared = !dev || !*dev;
	const bool dev_switched = s->usb_device[0] && strcmp(dev ? dev : "", s->usb_device) != 0;
	pthread_mutex_lock(&s->mtx);
	if (dev_cleared || dev_switched)
		s->usb_id[0] = 0;
	else if (!s->usb_id[0] && saved_id && *saved_id)
		snprintf(s->usb_id, sizeof(s->usb_id), "%s", saved_id);
	snprintf(s->usb_device, sizeof(s->usb_device), "%s", dev ? dev : "");
	pthread_mutex_unlock(&s->mtx);
	if (dev_cleared || dev_switched)
		obs_data_set_string(settings, "usb_device_id", "");
	s->bulk_raw_fallback = false;
	s->bulk_payload_fallback = false;
	s->backend = backend_string_to_kind(obs_data_get_string(settings, "uvc_backend"));

	s->width = (int)obs_data_get_int(settings, "width");
	s->height = (int)obs_data_get_int(settings, "height");
	s->fps = (int)obs_data_get_int(settings, "fps");

	const char *fmt = obs_data_get_string(settings, "video_format");
	snprintf(s->fmt_setting, sizeof(s->fmt_setting), "%s", fmt ? fmt : "");
	s->prefer_fmt = fmt_string_to_kind(fmt);
	const char *mode = obs_data_get_string(settings, "device_mode");
	snprintf(s->mode_setting, sizeof(s->mode_setting), "%s", mode);
	int kind, width, height;
	unsigned interval;
	char extra;
	s->requested_interval = 0;
	if (sscanf(s->mode_setting, "%d:%d:%d:%u%c", &kind, &width, &height, &interval, &extra) == 4 &&
	    kind >= MODE_MJPEG && kind <= MODE_UYVY && width >= 16 && width <= 7680 && height >= 16 &&
	    height <= 4320 && interval >= 41666 && interval <= 10000000) {
		s->prefer_fmt = kind;
		s->width = width;
		s->height = height;
		s->requested_interval = interval;
		s->fps = (int)((10000000U + interval / 2U) / interval);
		snprintf(s->fmt_setting, sizeof(s->fmt_setting), "%s", mode_kind_name((enum uvc_mode_kind)kind));
	}
	const enum video_colorspace cs = (enum video_colorspace)obs_data_get_int(settings, "yuv_colorspace");
	s->colorspace = cs == VIDEO_CS_601 || cs == VIDEO_CS_SRGB || cs == VIDEO_CS_2100_PQ || cs == VIDEO_CS_2100_HLG
				? cs : VIDEO_CS_709;
	memset(&s->raw_frame, 0, sizeof(s->raw_frame));
	s->audio_device = (int32_t)obs_data_get_int(settings, "audio_device");
	/* Keep the mixer's controls available while waiting for the USB audio route. */
	obs_source_set_audio_active(s->source, s->audio_device == UVC_AUDIO_AUTO || s->audio_device >= 0);
	s->yuv_low_latency = obs_data_get_bool(settings, "yuv_low_latency");
	uvc_apply_async_buffering(s, uvc_resolve_unbuffered(settings));

	if (s->width < 16 || s->width > 7680 || s->height < 16 || s->height > 4320) {
		blog(LOG_WARNING, "uvc: 配置分辨率 %dx%d 超出支持范围，已回退为 1920x1080", s->width, s->height);
		s->width = 1920;
		s->height = 1080;
	}
	/* 属性页是自由数字输入，步进 2 也能攒出奇数怪值（真机见过 1920x1082）——夹成偶数 */
	if (!s->requested_interval && !is_rgb24(s->prefer_fmt) && ((s->width | s->height) & 1)) {
		blog(LOG_WARNING, "uvc: 配置分辨率 %dx%d 非偶数，已对齐为 %dx%d", s->width, s->height, s->width & ~1,
		     s->height & ~1);
		s->width &= ~1;
		s->height &= ~1;
	}
	if (s->fps < 1 || s->fps > 240) {
		blog(LOG_WARNING, "uvc: 配置帧率 %d 超出有效范围，已回退为 60", s->fps);
		s->fps = 60;
	}
}

static enum video_range_type uvc_color_range(obs_data_t *settings)
{
	const int64_t range = obs_data_get_int(settings, "color_range");
	if (range == VIDEO_RANGE_PARTIAL || range == VIDEO_RANGE_FULL)
		return (enum video_range_type)range;
	return VIDEO_RANGE_DEFAULT;
}

static void *uvc_create(obs_data_t *settings, obs_source_t *source)
{
	struct uvc_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init_value(&s->mtx);
	if (pthread_mutex_init(&s->mtx, NULL) != 0)
		blog(LOG_ERROR, "uvc: 初始化互斥锁失败，状态文本更新将不受互斥保护");
	atomic_init(&s->color_range, uvc_color_range(settings));
	uvc_settings_apply(s, settings);
	return s;
}

static void uvc_destroy(void *data)
{
	struct uvc_source *s = data;
	if (!s)
		return;
	uvc_stop_thread(s);
	uvc_source_release_retire_gate(s);
	pthread_mutex_destroy(&s->mtx);
	bfree(s);
}

static void uvc_activate(void *data)
{
	uvc_start_thread(data);
}

static void uvc_deactivate(void *data)
{
	struct uvc_source *s = data;
	uvc_stop_thread(s);
	uvc_clear_negotiated_mode(s);
}

static int settings_effective_fmt_kind(obs_data_t *settings)
{
	const char *mode = obs_data_get_string(settings, "device_mode");
	if (mode && *mode) {
		int kind = -1, width = 0, height = 0;
		unsigned interval = 0;
		char extra = 0;
		if (sscanf(mode, "%d:%d:%d:%u%c", &kind, &width, &height, &interval, &extra) == 4 &&
		    kind >= MODE_MJPEG && kind <= MODE_UYVY && width >= 16 && width <= 7680 && height >= 16 &&
		    height <= 4320 && interval >= 41666 && interval <= 10000000)
			return kind;
	}
	return fmt_string_to_kind(obs_data_get_string(settings, "video_format"));
}

static void uvc_save(void *data, obs_data_t *settings)
{
	struct uvc_source *s = data;
	if (!s || !settings)
		return;
	pthread_mutex_lock(&s->mtx);
	if (s->usb_device[0])
		obs_data_set_string(settings, "usb_device", s->usb_device);
	if (s->usb_id[0])
		obs_data_set_string(settings, "usb_device_id", s->usb_id);
	pthread_mutex_unlock(&s->mtx);
}

static void uvc_update(void *data, obs_data_t *settings)
{
	struct uvc_source *s = data;
	const char *dev = obs_data_get_string(settings, "usb_device");
	const int new_w = (int)obs_data_get_int(settings, "width");
	const int new_h = (int)obs_data_get_int(settings, "height");
	const int new_fps = (int)obs_data_get_int(settings, "fps");
	const int new_fmt = fmt_string_to_kind(obs_data_get_string(settings, "video_format"));

	/* 只拿"设置项 vs 设置项"比，没变化就不重启（同 nikon-source 的判法） */
	const bool manual_change =
		!s->requested_interval && (new_w != s->width || new_h != s->height || new_fps != s->fps ||
					   new_fmt != s->prefer_fmt);
	const bool dev_changed = strcmp(dev ? dev : "", s->usb_device) != 0;
	bool path_synced = false;
	if (dev_changed && dev && *dev) {
		pthread_mutex_lock(&s->mtx);
		path_synced = s->mode_device[0] && strcmp(dev, s->mode_device) == 0 && s->usb_id[0];
		pthread_mutex_unlock(&s->mtx);
	}
	const enum uvc_backend_kind new_backend = backend_string_to_kind(obs_data_get_string(settings, "uvc_backend"));
	const bool backend_changed = new_backend != s->backend;
	const bool unbuffered = uvc_resolve_unbuffered(settings);
	const bool new_yuv_low_latency = obs_data_get_bool(settings, "yuv_low_latency");
	const bool new_raw_ll = new_yuv_low_latency && is_yuv_mode_kind(settings_effective_fmt_kind(settings));
	const bool raw_ll_changed = new_raw_ll != raw_low_latency(s);
	bool changed = (dev_changed && !path_synced) || backend_changed || manual_change ||
		       strcmp(obs_data_get_string(settings, "device_mode"), s->mode_setting) != 0 ||
		       raw_ll_changed ||
		       obs_data_get_int(settings, "yuv_colorspace") != s->colorspace ||
		       obs_data_get_int(settings, "audio_device") != s->audio_device;
	if (changed) {
		uvc_stop_thread(s);
		uvc_clear_negotiated_mode(s);
		if (backend_changed)
			s->backend_switch_settle = true;
		if ((dev_changed && !path_synced) || backend_changed) {
			pthread_mutex_lock(&s->mtx);
			s->device_mode_count = 0;
			s->mode_device[0] = '\0';
			pthread_mutex_unlock(&s->mtx);
		}
	}
	atomic_store_explicit(&s->color_range, uvc_color_range(settings), memory_order_relaxed);
	if (!changed) {
		s->yuv_low_latency = new_yuv_low_latency;
		if (path_synced) {
			pthread_mutex_lock(&s->mtx);
			snprintf(s->usb_device, sizeof(s->usb_device), "%s", dev);
			pthread_mutex_unlock(&s->mtx);
		}
		uvc_apply_async_buffering(s, unbuffered);
		return;
	}

	uvc_settings_apply(s, settings);
	if (obs_source_active(s->source))
		uvc_start_thread(s);
}

static bool resolve_usb_device_path(struct uvc_source *s, const char *path, char resolved[OBS_ANDROID_USB_NAME_LEN]);

/* 属性页【申请 USB 授权】：权限结果是异步的，返回 false 不刷属性页（同 nikon-source） */
static bool on_request_permission(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct uvc_source *s = data;

	if (!s || !s->usb_device[0]) {
		blog(LOG_INFO, "uvc: 尚未选择 USB 设备，请先选择设备再申请授权");
		return false;
	}
	char target[OBS_ANDROID_USB_NAME_LEN];
	if (!resolve_usb_device_path(s, s->usb_device, target))
		snprintf(target, sizeof(target), "%s", s->usb_device);
	const int r = obs_android_usb_request_permission(target);
	blog(LOG_INFO, "uvc: 属性页对 '%s' 点【申请授权】，总线返回 %d（%s）", target, r,
	     r == 1   ? "弹窗中，用户选完后重新激活本源生效"
	     : r == 0 ? "其实已有权限"
		      : "失败");
	return false;
}

/* Resolve an older stored USB path to the live device path after replug. */
static bool resolve_usb_device_path(struct uvc_source *s, const char *path, char resolved[OBS_ANDROID_USB_NAME_LEN])
{
	resolved[0] = '\0';
	if (!path || !*path)
		return false;
	snprintf(resolved, OBS_ANDROID_USB_NAME_LEN, "%s", path);

	char usb_id[sizeof(s->usb_id)] = {0};
	char mode_device[OBS_ANDROID_USB_NAME_LEN] = {0};
	if (s) {
		pthread_mutex_lock(&s->mtx);
		if (strcmp(path, s->usb_device) == 0) {
			memcpy(usb_id, s->usb_id, sizeof(usb_id));
			memcpy(mode_device, s->mode_device, sizeof(mode_device));
		}
		pthread_mutex_unlock(&s->mtx);
	}

	struct obs_android_usb_device devs[8];
	const int count = obs_android_usb_enum_devices(devs, (int)(sizeof(devs) / sizeof(devs[0])));
	if (count > 0) {
		const int selected = find_usb_device(devs, count, path, usb_id);
		if (selected >= 0)
			snprintf(resolved, OBS_ANDROID_USB_NAME_LEN, "%s", devs[selected].name);
	} else if (count < 0 && mode_device[0]) {
		snprintf(resolved, OBS_ANDROID_USB_NAME_LEN, "%s", mode_device);
	}
	return true;
}

/* The USB host rejects descriptor leases while another source owns the device. */
static int read_device_modes(struct uvc_source *s, const char *name, enum uvc_backend_kind backend,
			     struct uvc_mode modes[MAX_MODES])
{
	char resolved[OBS_ANDROID_USB_NAME_LEN];
	if (!resolve_usb_device_path(s, name, resolved))
		return 0;

	if (s) {
		pthread_mutex_lock(&s->mtx);
		const int count =
			(strcmp(resolved, s->mode_device) == 0 && s->mode_backend == backend) ? s->device_mode_count : 0;
		if (count)
			memcpy(modes, s->device_modes, (size_t)count * sizeof(*modes));
		pthread_mutex_unlock(&s->mtx);
		if (count)
			return count;
	}

	if (s && (uvc_retire_is_pending(s) || os_atomic_load_long(&s->thread_started)))
		return 0;

	const int fd = obs_android_usb_open(resolved);
	if (fd < 0)
		return 0;
	int count = 0;
	if (backend == UVC_BACKEND_LIBUVC) {
		struct uvc_libuvc_mode lmodes[MAX_MODES];
		const int lc = uvc_libuvc_probe_modes(fd, p010_available, lmodes, MAX_MODES);
		if (lc > 0) {
			convert_libuvc_modes(lmodes, lc, modes);
			count = lc;
		}
	}
	if (count == 0) {
		struct uvc_dev device = {0};
		const struct libusb_init_option options[] = {{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}}};
		if (libusb_init_context(&device.ctx, options, 1) == 0) {
			if (libusb_wrap_sys_device(device.ctx, (intptr_t)fd, &device.h) == 0) {
				if (uvc_scan_device(&device)) {
					device.quirks = device_quirks(device.h);
					count = copy_usable_modes(modes, &device);
				}
				libusb_close(device.h);
			}
			libusb_exit(device.ctx);
		}
	}
	obs_android_usb_close(fd);
	return count;
}

static void add_mode_option(obs_property_t *list, const struct uvc_mode *mode, uint32_t interval)
{
	if (interval < 41666 || interval > 10000000 || !mode_has_interval(mode, interval) || mode->w < 16 ||
	    mode->w > 7680 || mode->h < 16 || mode->h > 4320 || raw_frame_bytes(mode) > MAX_FRAME_BYTES)
		return;
	char value[48];
	snprintf(value, sizeof(value), "%d:%u:%u:%u", (int)mode->kind, (unsigned)mode->w, (unsigned)mode->h,
		 (unsigned)interval);
	for (size_t i = 0; i < obs_property_list_item_count(list); ++i) {
		if (strcmp(obs_property_list_item_string(list, i), value) == 0)
			return;
	}
	struct dstr label;
	uvc_format_mode_label(&label, mode->kind, mode->w, mode->h, interval);
	obs_property_list_add_string(list, label.array, value);
	dstr_free(&label);
}

static void update_yuv_option_visibility(obs_properties_t *props, obs_data_t *settings)
{
	const bool yuv = is_yuv_mode_kind(settings_effective_fmt_kind(settings));
	obs_property_t *latency = obs_properties_get(props, "yuv_low_latency");
	if (latency)
		obs_property_set_visible(latency, yuv);
}

static bool on_yuv_options_change(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	update_yuv_option_visibility(props, settings);
	return true;
}

static bool on_mode_change(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool manual = !*obs_data_get_string(settings, "device_mode");
	obs_property_set_visible(obs_properties_get(props, "width"), manual);
	obs_property_set_visible(obs_properties_get(props, "height"), manual);
	obs_property_set_visible(obs_properties_get(props, "fps"), manual);
	obs_property_set_visible(obs_properties_get(props, "video_format"), manual);
	update_yuv_option_visibility(props, settings);
	return true;
}

static void load_audio_inputs(struct uvc_source *s, obs_property_t *list, const char *path)
{
	int matched = -1;
	char resolved[OBS_ANDROID_USB_NAME_LEN];
	if (resolve_usb_device_path(s, path, resolved))
		matched = obs_android_audio_find_usb_input(resolved);

	obs_property_list_clear(list);
	obs_property_list_add_int(list, obs_module_text("AudioInput.Disabled"), UVC_AUDIO_DISABLED);
	obs_property_list_add_int(list, obs_module_text("AudioInput.Default"), UVC_AUDIO_SYSTEM_DEFAULT);

	struct obs_android_audio_device devices[32];
	const char *product = NULL;
	const int count = obs_android_audio_enum_devices(devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < count; ++i) {
		const struct obs_android_audio_device *device = &devices[i];
		if (!device->is_source || device->id <= 0)
			continue;
		obs_property_list_add_int(list, device->label, device->id);
		if (device->id == matched)
			product = device->product;
	}

	if (!product) {
		obs_property_list_insert_int(list, 0, obs_module_text("AudioInput.AutoNoMatch"), UVC_AUDIO_AUTO);
		return;
	}

	char name[OBS_ANDROID_AUDIO_PRODUCT_LEN + 24];
	if (*product)
		snprintf(name, sizeof(name), "%s (ID %d)", product, matched);
	else
		snprintf(name, sizeof(name), "ID %d", matched);
	const char *args[] = {name};
	struct dstr label;
	obs_android_format_locale(&label, obs_module_text("AudioInput.Auto"), args, 1);
	obs_property_list_insert_int(list, 0, label.array, UVC_AUDIO_AUTO);
	dstr_free(&label);
}

static bool on_usb_device_change(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	struct uvc_source *s = data;
	const char *name = obs_data_get_string(settings, "usb_device");
	char resolved[OBS_ANDROID_USB_NAME_LEN] = {0};
	if (resolve_usb_device_path(s, name, resolved) && strcmp(resolved, name) != 0)
		obs_data_set_string(settings, "usb_device", resolved);
	const char *effective = resolved[0] ? resolved : name;
	const char *selected = obs_data_get_string(settings, "device_mode");

	bool neg_valid = false;
	enum uvc_mode_kind neg_kind = MODE_MJPEG;
	uint16_t neg_w = 0;
	uint16_t neg_h = 0;
	uint32_t neg_iv = 0;
	char neg_dev[OBS_ANDROID_USB_NAME_LEN] = {0};
	if (s) {
		pthread_mutex_lock(&s->mtx);
		neg_valid = s->negotiated_valid;
		neg_kind = s->negotiated_kind;
		neg_w = s->negotiated_w;
		neg_h = s->negotiated_h;
		neg_iv = s->negotiated_interval;
		memcpy(neg_dev, s->negotiated_device, sizeof(neg_dev));
		pthread_mutex_unlock(&s->mtx);
	}

	obs_property_t *list = obs_properties_get(props, "device_mode");
	obs_property_list_clear(list);
	if (!*selected && neg_valid && *effective && strcmp(neg_dev, effective) == 0 && neg_iv > 0) {
		struct dstr mode_label;
		uvc_format_mode_label(&mode_label, neg_kind, neg_w, neg_h, neg_iv);
		const char *args[] = {mode_label.array ? mode_label.array : ""};
		struct dstr manual_label;
		obs_android_format_locale(&manual_label, obs_module_text("DeviceMode.ManualNegotiated"), args, 1);
		obs_property_list_add_string(
			list, manual_label.array ? manual_label.array : obs_module_text("DeviceMode.Manual"), "");
		dstr_free(&manual_label);
		dstr_free(&mode_label);
	} else {
		obs_property_list_add_string(list, obs_module_text("DeviceMode.Manual"), "");
	}
	if (*effective) {
		struct uvc_mode modes[MAX_MODES];
		const enum uvc_backend_kind sel_backend =
			backend_string_to_kind(obs_data_get_string(settings, "uvc_backend"));
		const int count = read_device_modes(s, effective, sel_backend, modes);
		for (int i = 0; i < count; ++i) {
			const struct uvc_mode *mode = &modes[i];
			for (int j = 0; j < mode->n_intervals; ++j)
				add_mode_option(list, mode, mode->intervals[j]);
			if (!mode->n_intervals) {
				add_mode_option(list, mode, mode->def_interval);
				add_mode_option(list, mode, mode->min_iv);
				add_mode_option(list, mode, mode->max_iv);
				const int rates[] = {24, 25, 30, 50, 60, 120, 240};
				for (size_t j = 0; j < sizeof(rates) / sizeof(*rates); ++j)
					add_mode_option(list, mode, pick_interval(mode, rates[j]));
			}
		}
	}
	if (*selected) {
		bool found = false;
		for (size_t i = 1; i < obs_property_list_item_count(list); ++i)
			found |= strcmp(obs_property_list_item_string(list, i), selected) == 0;
		if (!found) {
			const size_t item = obs_property_list_add_string(list, obs_module_text("DeviceMode.Unavailable"),
									 selected);
			obs_property_list_item_disable(list, item, true);
		}
	}
	load_audio_inputs(s, obs_properties_get(props, "audio_device"), effective);
	return on_mode_change(props, list, settings);
}

static obs_properties_t *uvc_properties(void *data)
{
	struct uvc_source *s = data;
	obs_properties_t *props = obs_properties_create();

	/* 设备清单是打开属性页那一刻的快照（插拔后要重开对话框） */
	obs_property_t *dev_list = obs_properties_add_list(props, "usb_device", obs_module_text("UsbDevice"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	struct obs_android_usb_device devs[8];
	const int n = obs_android_usb_enum_devices(devs, 8);
	char saved_device[OBS_ANDROID_USB_NAME_LEN] = {0};
	char saved_id[sizeof(s->usb_id)] = {0};
	if (s) {
		pthread_mutex_lock(&s->mtx);
		snprintf(saved_device, sizeof(saved_device), "%s", s->usb_device);
		snprintf(saved_id, sizeof(saved_id), "%s", s->usb_id);
		pthread_mutex_unlock(&s->mtx);
	}
	const bool saved_present = saved_device[0] && n > 0 && find_usb_device(devs, n, saved_device, saved_id) >= 0;
	if (n < 0) {
		obs_property_list_add_string(dev_list, obs_module_text("UsbDevice.BusNotReady"), "");
		blog(LOG_WARNING, "uvc: 属性页打不开设备清单 —— USB 总线未注册");
	} else if (n == 0) {
		obs_property_list_add_string(dev_list, obs_module_text("UsbDevice.None"), "");
	} else {
		for (int i = 0; i < n; i++) {
			char text[OBS_ANDROID_USB_LABEL_LEN + OBS_ANDROID_USB_NAME_LEN + 64];
			snprintf(text, sizeof(text), "%s [%s]%s", devs[i].label, devs[i].name,
				 devs[i].has_permission ? "" : obs_module_text("UsbDevice.UnauthorizedSuffix"));
			obs_property_list_add_string(dev_list, text, devs[i].name);
		}
		obs_property_list_add_string(dev_list, obs_module_text("UsbDevice.Disabled"), "");
		blog(LOG_INFO, "uvc: 属性页枚举到 %d 台 USB 设备", n);
	}
	/* Keep a disconnected saved device in the list (disabled) so opening Properties
	 * while the capture card is temporarily unplugged does not wipe usb_device to "". */
	if (saved_device[0] && !saved_present) {
		char text[OBS_ANDROID_USB_NAME_LEN + 96];
		if (saved_id[0])
			snprintf(text, sizeof(text), "%s [%s]%s", saved_id, saved_device,
				 obs_module_text("UsbDevice.DisconnectedSuffix"));
		else
			snprintf(text, sizeof(text), "%s%s", saved_device,
				 obs_module_text("UsbDevice.DisconnectedSuffix"));
		const size_t item = obs_property_list_add_string(dev_list, text, saved_device);
		obs_property_list_item_disable(dev_list, item, true);
	}

	obs_properties_add_button2(props, "request_perm", obs_module_text("RequestPerm"), on_request_permission, s);

	obs_property_t *backend = obs_properties_add_list(props, "uvc_backend", obs_module_text("Backend"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(backend, obs_module_text("Backend.Native"), "native");
	obs_property_list_add_string(backend, obs_module_text("Backend.Libuvc"), "libuvc");
	obs_property_set_long_description(backend, obs_module_text("Backend.Description"));
	obs_property_set_modified_callback2(backend, on_usb_device_change, s);

	obs_property_t *mode = obs_properties_add_list(props, "device_mode", obs_module_text("DeviceMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("DeviceMode.Manual"), "");
	obs_property_set_long_description(mode, obs_module_text("DeviceMode.Description"));
	obs_property_set_modified_callback(mode, on_mode_change);
	obs_property_set_modified_callback2(dev_list, on_usb_device_change, s);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 16, 7680, 2);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 16, 4320, 2);
	obs_properties_add_int(props, "fps", obs_module_text("Fps"), 1, 240, 1);

	obs_property_t *p = obs_properties_add_list(props, "video_format", obs_module_text("VideoFormat"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Mjpeg"), "mjpeg");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Yuyv"), "yuyv");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Uyvy"), "uyvy");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Nv12"), "nv12");
	if (p010_available)
		obs_property_list_add_string(p, "P010 (10-bit)", "p010");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Rgb24"), "rgb24");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Bgr24"), "bgr24");
	obs_property_list_add_string(p, obs_module_text("VideoFormat.Auto"), "");
	obs_property_set_long_description(p, obs_module_text("VideoFormat.Description"));
	obs_property_set_modified_callback(p, on_yuv_options_change);

	obs_property_t *color = obs_properties_add_list(props, "yuv_colorspace", obs_module_text("YuvColorSpace"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(color, "Rec. 709", VIDEO_CS_709);
	obs_property_list_add_int(color, "Rec. 601", VIDEO_CS_601);
	obs_property_list_add_int(color, "sRGB", VIDEO_CS_SRGB);
	if (p010_available) {
		obs_property_list_add_int(color, "Rec. 2100 (PQ)", VIDEO_CS_2100_PQ);
		obs_property_list_add_int(color, "Rec. 2100 (HLG)", VIDEO_CS_2100_HLG);
	}

	obs_property_t *range = obs_properties_add_list(props, "color_range", obs_module_text("ColorRange"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(range, obs_module_text("ColorRange.Default"), VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(range, obs_module_text("ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(range, obs_module_text("ColorRange.Full"), VIDEO_RANGE_FULL);
	obs_property_set_long_description(range, obs_module_text("ColorRange.Description"));

	obs_property_t *audio = obs_properties_add_list(props, "audio_device", obs_module_text("AudioInput"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(audio, obs_module_text("AudioInput.Description"));

	obs_property_t *latency = obs_properties_add_bool(props, "yuv_low_latency", obs_module_text("YuvPassthrough"));
	obs_property_set_long_description(
		latency,
		obs_module_text("YuvPassthrough.Description"));
	obs_property_set_modified_callback(latency, on_yuv_options_change);

	obs_property_t *unbuffered =
		obs_properties_add_bool(props, "low_latency_unbuffered", obs_module_text("YuvUnbuffered"));
	obs_property_set_long_description(unbuffered, obs_module_text("YuvUnbuffered.Description"));

	if (s) {
		struct dstr state = {0};
		pthread_mutex_lock(&s->mtx);
		if (s->status[0])
			dstr_copy(&state, s->status);
		pthread_mutex_unlock(&s->mtx);
		if (dstr_is_empty(&state)) {
			char frames[24];
			snprintf(frames, sizeof(frames), "%llu", (unsigned long long)s->frames_out);
			const char *args[] = {frames};
			obs_android_format_locale(&state, obs_module_text("State.Idle"), args, 1);
		}
		obs_properties_add_text(props, "cap_state", state.array ? state.array : "", OBS_TEXT_INFO);
		dstr_free(&state);
	}

	return props;
}

struct obs_source_info uvc_input_info = {
	.id = "android_video_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* 异步源：靠 obs_source_output_video 推帧，宽高由 libobs 从帧里推 */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = uvc_getname,
	.create = uvc_create,
	.destroy = uvc_destroy,
	.activate = uvc_activate,
	.deactivate = uvc_deactivate,
	.update = uvc_update,
	.save = uvc_save,
	.get_defaults = uvc_get_defaults,
	.get_properties = uvc_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-capture", "en-US")

bool obs_module_load(void)
{
	obs_enter_graphics();
	p010_available = gs_p010_available();
	obs_leave_graphics();
	const struct libusb_version *uv = libusb_get_version();

	/* obs_register_source 返回 void，注册被拒只会在 blog 里说一声，必须自己回读确认 */
	obs_register_source(&uvc_input_info);

	const uint32_t flags = obs_get_source_output_flags("android_video_input");
	if (!flags) {
		blog(LOG_ERROR, "android-capture: 注册源类型 'android_video_input' 失败，请查看 libobs 错误日志");
		return false;
	}

	blog(LOG_INFO,
	     "android-capture: 已注册源类型 'android_video_input'（flags=0x%x / 双后端：自研原生 UVC + Android 补丁版 libuvc 0.0.9 / "
	     "libusb %u.%u.%u.%u api=0x%x）",
	     (unsigned)flags, (unsigned)uv->major, (unsigned)uv->minor, (unsigned)uv->micro, (unsigned)uv->nano,
	     (unsigned)LIBUSB_API_VERSION);
	return true;
}
