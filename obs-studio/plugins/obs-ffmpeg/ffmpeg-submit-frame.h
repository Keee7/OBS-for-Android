#pragma once

/* OBS frame PTS values already advance by fps_den each frame. */
static uint64_t ffmpeg_pts_to_ns(uint64_t pts, uint64_t timebase_den)
{
	return util_mul_div64(pts, UINT64_C(1000000000), timebase_den);
}

/* One OBS input frame can return at most one packet. If the encoder applies
 * backpressure, deliver its pending packet and retry this same frame once. */
static int ffmpeg_submit_frame(AVCodecContext *context, AVFrame *frame, AVPacket *packet, bool *received)
{
	int ret;
	*received = false;
	ret = avcodec_send_frame(context, frame);
	if (ret == AVERROR(EAGAIN)) {
		ret = avcodec_receive_packet(context, packet);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(context, frame);
		if (ret < 0) {
			av_packet_unref(packet);
			return ret;
		}
		*received = true;
		return 0;
	}
	if (ret < 0)
		return ret;
	ret = avcodec_receive_packet(context, packet);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		return 0;
	if (ret < 0)
		return ret;
	*received = true;
	return 0;
}
