/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../avc-utils.h"
#include <assert.h>
#include <string.h>
int main(void)
{
	uint8_t both[] = {0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x68, 0x11};
	assert(avc_to_annexb(both, sizeof both));
	assert(avc_has_nal(both, sizeof both, 7) && avc_has_nal(both, sizeof both, 8));
	uint8_t three[] = {0, 0, 1, 0x65, 42};
	assert(avc_to_annexb(three, sizeof three));
	assert(avc_has_nal(three, sizeof three, 5));
	uint8_t bad[] = {0, 0, 0, 8, 0x67};
	assert(!avc_to_annexb(bad, sizeof bad));
	uint8_t zero[] = {0, 0, 0, 0};
	assert(!avc_to_annexb(zero, sizeof zero));
	uint8_t short_data[] = {0, 0, 1};
	assert(!avc_to_annexb(short_data, sizeof short_data));
	uint8_t high8[] = {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0xac};
	uint8_t high10[] = {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0xa5, 0};
	assert(avc_sps_8bit_420(high8, sizeof high8));
	assert(!avc_sps_8bit_420(high10, sizeof high10));
	assert(!avc_sps_8bit_420(high8, 7));
	for (size_t i = 0; i < 8; ++i)
		assert(!avc_sps_8bit_420(high8, i));
	int64_t ns, pts;
	assert(avc_pts_to_ns(1001, 30000, &ns) && ns == 33366666);
	assert(avc_us_to_pts(ns / 1000, 30000, &pts) && pts == 1001);
	assert(avc_pts_to_ns(3600 * 60, 60, &ns) && ns == 3600000000000LL);
	assert(!avc_pts_to_ns(-1, 60, &ns) && !avc_us_to_pts(-1, 60, &pts));
	assert(!avc_pts_to_ns(INT64_MAX, 1, &ns));
	return 0;
}
