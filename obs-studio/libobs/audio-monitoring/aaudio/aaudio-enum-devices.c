#include "../../obs-internal.h"
#include "../../obs-android.h"

#include <stdio.h>
#include <string.h>

#define AAUDIO_MON_DEV_MAX 32

void obs_enum_audio_monitoring_devices(obs_enum_audio_device_cb cb, void *data)
{
	if (!cb)
		return;

	struct obs_android_audio_device devs[AAUDIO_MON_DEV_MAX];
	int n = obs_android_audio_enum_output_devices(devs, AAUDIO_MON_DEV_MAX);
	if (n <= 0)
		return;

	for (int i = 0; i < n; i++) {
		if (devs[i].id <= 0 || !devs[i].is_sink)
			continue;

		char id_str[16];
		snprintf(id_str, sizeof(id_str), "%d", (int)devs[i].id);
		const char *label = devs[i].label[0] ? devs[i].label
						     : (devs[i].product[0] ? devs[i].product : id_str);
		if (!cb(data, label, id_str))
			break;
	}
}

bool devices_match(const char *id1, const char *id2)
{
	if (!id1 || !id2)
		return false;

	return strcmp(id1, id2) == 0;
}
