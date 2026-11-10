#include <assert.h>
#include <stdint.h>
#include "../uvc-cadence-stats.h"
int main(void)
{
	uint8_t yuyv[64 * 64 * 2] = {0}, nv12[64 * 64] = {0};
	struct uvc_cadence_stats s = {0};
	uvc_cadence_observe(&s, yuyv, 64, 64, true, 100000000);
	yuyv[1] = 42; /* Chroma is excluded. */
	uvc_cadence_observe(&s, yuyv, 64, 64, true, 116666667);
	assert(s.same_hash == 1 && s.dt[2] == 1);
	yuyv[2] = 1;
	uvc_cadence_observe(&s, yuyv, 64, 64, true, 116666667);
	assert(s.same_hash == 1 && s.dt[0] == 1);
	uvc_cadence_reset_window(&s);
	uvc_cadence_observe(&s, nv12, 64, 64, false, 150000000);
	assert(s.samples == 1 && s.same_hash == 0 && s.dt[4] == 1);
	uvc_cadence_observe_pts(&s, true, UINT32_MAX - 2);
	uvc_cadence_observe_pts(&s, true, 2);
	uvc_cadence_observe_pts(&s, true, 2);
	assert(s.pts_frames == 3 && s.pts_pairs == 2 && s.pts_wraps == 1 && s.pts_same == 1);
	assert(s.pts_delta_min == 0 && s.pts_delta_max == 5);
	uvc_cadence_observe_pts(&s, false, 0);
	uvc_cadence_observe_pts(&s, true, 10);
	assert(s.pts_pairs == 2);
	return 0;
}
