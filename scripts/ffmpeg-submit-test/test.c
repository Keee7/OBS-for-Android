#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "../../obs-studio/libobs/util/util_uint64.h"
#include <errno.h>
#define AVERROR(e) (-(e))
#define AVERROR_EOF (-1000)
typedef struct { int unused; } AVCodecContext;
typedef struct { int unused; } AVFrame;
typedef struct { int data; } AVPacket;
static int sends[2], receives[1], send_count, receive_count, released;
static int avcodec_send_frame(AVCodecContext *context, AVFrame *frame)
{
	(void)context; (void)frame;
	assert(send_count < 2);
	return sends[send_count++];
}
static int avcodec_receive_packet(AVCodecContext *context, AVPacket *packet)
{
	(void)context;
	assert(receive_count < 1);
	if (receives[receive_count] == 0)
		packet->data = 42;
	return receives[receive_count++];
}
static void av_packet_unref(AVPacket *packet) { packet->data = 0; released++; }
#include "../../obs-studio/plugins/obs-ffmpeg/ffmpeg-submit-frame.h"
static void reset(void) { send_count = receive_count = released = 0; sends[0] = sends[1] = receives[0] = 0; }
int main(void)
{
	AVCodecContext context = {0}; AVFrame frame = {0}; AVPacket packet = {0}; bool got;
	/* OBS supplies 1001 PTS ticks per frame for 60000/1001 and 30000/1001. */
	assert(ffmpeg_pts_to_ns(1001, 60000) == UINT64_C(16683333));
	assert(ffmpeg_pts_to_ns(1001, 30000) == UINT64_C(33366666));
	assert(ffmpeg_pts_to_ns(60000ULL * 1001, 60000) == UINT64_C(1001000000000));
	reset(); sends[0] = AVERROR(EAGAIN);
	assert(ffmpeg_submit_frame(&context, &frame, &packet, &got) == 0);
	assert(got && packet.data == 42 && send_count == 2 && receive_count == 1 && !released);
	reset(); sends[0] = AVERROR(EAGAIN); sends[1] = AVERROR(EAGAIN); packet.data = 0;
	assert(ffmpeg_submit_frame(&context, &frame, &packet, &got) == AVERROR(EAGAIN));
	assert(!got && !packet.data && send_count == 2 && released == 1);
	reset(); receives[0] = AVERROR(EAGAIN);
	assert(ffmpeg_submit_frame(&context, &frame, &packet, &got) == 0 && !got && send_count == 1);
	reset(); sends[0] = AVERROR(EAGAIN); receives[0] = AVERROR(EAGAIN);
	assert(ffmpeg_submit_frame(&context, &frame, &packet, &got) == AVERROR(EAGAIN) && !got && send_count == 1);
	reset(); sends[0] = -5;
	assert(ffmpeg_submit_frame(&context, &frame, &packet, &got) == -5 && !got && receive_count == 0);
	return 0;
}
