#include "obs-ffmpeg-video-encoders.h"
#include <stdio.h>

struct x265_encoder {
	struct ffmpeg_video_encoder ffve;
};

static const char *x265_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Software HEVC (x265)";
}

static void x265_video_info(void *unused, struct video_scale_info *info)
{
	UNUSED_PARAMETER(unused);
	info->format = VIDEO_FORMAT_I420;
}

static void x265_destroy(void *data)
{
	struct x265_encoder *enc = data;
	ffmpeg_video_encoder_free(&enc->ffve);
	bfree(enc);
}

static void *x265_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct x265_encoder *enc = bzalloc(sizeof(*enc));
	if (!ffmpeg_video_encoder_init(&enc->ffve, enc, encoder, "libx265", NULL, "x265", NULL, NULL))
		goto fail;
	const struct video_output_info *voi = video_output_get_info(obs_encoder_video(encoder));
	if (voi->colorspace == VIDEO_CS_2100_PQ || voi->colorspace == VIDEO_CS_2100_HLG) {
		obs_encoder_set_last_error(encoder, "This x265 build supports 8-bit SDR only.");
		goto fail;
	}
	struct video_scale_info info = {0};
	info.format = VIDEO_FORMAT_I420;
	info.colorspace = voi->colorspace;
	info.range = voi->range;
	const char *rc = obs_data_get_string(settings, "rate_control");
	int64_t bitrate_value = obs_data_get_int(settings, "bitrate");
	int64_t threads_value = obs_data_get_int(settings, "threads");
	int64_t pool_value = obs_data_get_int(settings, "pool_threads");
	int64_t keyint = obs_data_get_int(settings, "keyint_sec");
	int64_t crf = obs_data_get_int(settings, "crf");
	const char *preset = obs_data_get_string(settings, "preset");
	bool valid_rc = astrcmpi(rc, "CBR") == 0 || astrcmpi(rc, "VBR") == 0 || astrcmpi(rc, "CQP") == 0;
	bool valid_preset = strcmp(preset, "ultrafast") == 0 || strcmp(preset, "superfast") == 0 ||
			    strcmp(preset, "veryfast") == 0 || strcmp(preset, "faster") == 0 ||
			    strcmp(preset, "fast") == 0;
	if (!valid_rc || !valid_preset || bitrate_value < 50 || bitrate_value > 300000 ||
	    threads_value < 0 || threads_value > 64 || pool_value < 0 || pool_value > 64 ||
	    keyint < 0 || keyint > 20 || crf < 0 || crf > 51) {
		obs_encoder_set_last_error(encoder, "Invalid x265 rate control, bitrate, preset, keyframe interval or thread count.");
		goto fail;
	}
	int bitrate = (int)bitrate_value;
	enc->ffve.context->thread_count = (int)threads_value;
	int pool_threads = (int)pool_value;
	char params[128] = {0};
	if (pool_threads > 0)
		snprintf(params, sizeof(params), "pools=%d%s", pool_threads,
			 astrcmpi(rc, "CBR") == 0 ? ":strict-cbr=1:hrd=1" : "");
	else if (astrcmpi(rc, "CBR") == 0)
		snprintf(params, sizeof(params), "strict-cbr=1:hrd=1");
	if ((*params && av_opt_set(enc->ffve.context->priv_data, "x265-params", params, 0) < 0) ||
	    av_opt_set(enc->ffve.context->priv_data, "preset", preset, 0) < 0) {
		obs_encoder_set_last_error(encoder, "Failed to configure x265 preset or options.");
		goto fail;
	}
	if (astrcmpi(rc, "CQP") == 0) {
		bitrate = 0;
		if (av_opt_set_int(enc->ffve.context->priv_data, "crf", crf, 0) < 0) {
			obs_encoder_set_last_error(encoder, "Failed to configure x265 CRF.");
			goto fail;
		}
	} else if (astrcmpi(rc, "CBR") == 0) {
		enc->ffve.context->rc_max_rate = (int64_t)bitrate * 1000;
		enc->ffve.context->rc_min_rate = (int64_t)bitrate * 1000;
	}
	struct obs_options options = obs_parse_options(obs_data_get_string(settings, "ffmpeg_opts"));
	bool valid_options = true;
	for (size_t i = 0; i < options.count; i++) {
		if (av_opt_set(enc->ffve.context->priv_data, options.options[i].name, options.options[i].value, 0) < 0) {
			blog(LOG_ERROR, "Unknown or invalid x265 FFmpeg option: %s", options.options[i].name);
			valid_options = false;
			break;
		}
	}
	obs_free_options(options);
	if (!valid_options) {
		obs_encoder_set_last_error(encoder, "Invalid x265 FFmpeg option; see the OBS log.");
		goto fail;
	}
	ffmpeg_video_encoder_update(&enc->ffve, bitrate, (int)keyint, voi, &info, "");
	enc->ffve.context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (!ffmpeg_video_encoder_init_codec(&enc->ffve))
		goto fail;
	return enc;
fail:
	x265_destroy(enc);
	return NULL;
}

static bool x265_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
	struct x265_encoder *enc = data;
	return ffmpeg_video_encode(&enc->ffve, frame, packet, received);
}

static bool x265_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct x265_encoder *enc = data;
	*extra_data = enc->ffve.context->extradata;
	*size = enc->ffve.context->extradata_size;
	return *size > 0;
}

static void x265_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 6000);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "crf", 23);
	obs_data_set_default_int(settings, "threads", 0);
	obs_data_set_default_int(settings, "pool_threads", 0);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "ultrafast");
}

static bool x265_rate_control_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool crf = astrcmpi(obs_data_get_string(settings, "rate_control"), "CQP") == 0;
	obs_property_set_visible(obs_properties_get(props, "bitrate"), !crf);
	obs_property_set_visible(obs_properties_get(props, "crf"), crf);
	return true;
}

static obs_properties_t *x265_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "rate_control", obs_module_text("RateControl"),
						   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "VBR", "VBR");
	obs_property_list_add_string(p, "CRF", "CQP");
	obs_property_set_modified_callback(p, x265_rate_control_modified);
	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"), 50, 300000, 50);
	obs_property_int_set_suffix(p, " Kbps");
	obs_properties_add_int(props, "crf", "CRF", 0, 51, 1);
	p = obs_properties_add_int(props, "keyint_sec", obs_module_text("KeyframeIntervalSec"), 0, 20, 1);
	obs_property_int_set_suffix(p, " s");
	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "ultrafast", "ultrafast");
	obs_property_list_add_string(p, "superfast", "superfast");
	obs_property_list_add_string(p, "veryfast", "veryfast");
	obs_property_list_add_string(p, "faster", "faster");
	obs_property_list_add_string(p, "fast", "fast");
	obs_properties_add_int(props, "threads", "Frame threads (0 = automatic)", 0, 64, 1);
	obs_properties_add_int(props, "pool_threads", "Worker threads (0 = automatic)", 0, 64, 1);
	obs_properties_add_text(props, "ffmpeg_opts", obs_module_text("FFmpegOpts"), OBS_TEXT_DEFAULT);
	return props;
}

struct obs_encoder_info x265_encoder_info = {
	.id = "ffmpeg_x265",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = x265_name,
	.create = x265_create,
	.destroy = x265_destroy,
	.encode = x265_encode,
	.get_defaults = x265_defaults,
	.get_properties = x265_properties,
	.get_extra_data = x265_extra_data,
	.get_video_info = x265_video_info,
};
