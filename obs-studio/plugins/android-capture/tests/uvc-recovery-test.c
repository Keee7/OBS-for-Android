/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "uvc-recovery.h"
#include <assert.h>
#include <stdio.h>

#define SEC 1000000000ULL
int main(void)
{
	struct uvc_recovery r;
	struct uvc_reset_limit limit = {0};
	uvc_recovery_start(&r, 0);
	assert(uvc_recovery_poll(&r, 8 * SEC - 1) == UVC_RECOVERY_WAIT);
	assert(uvc_recovery_poll(&r, 8 * SEC) == UVC_RECOVERY_RESTART);
	/* Restart control took 25 seconds: deadline begins after it finishes. */
	uvc_recovery_restart_finished(&r, 33 * SEC);
	assert(uvc_recovery_poll(&r, 40 * SEC) == UVC_RECOVERY_WAIT);
	assert(uvc_recovery_poll(&r, 41 * SEC) == UVC_RECOVERY_RESET);
	assert(uvc_recovery_allow_reset(&limit, 41 * SEC));
	/* A fresh session must not reset again before the shared deadline. */
	uvc_recovery_start(&r, 42 * SEC);
	assert(uvc_recovery_poll(&r, 50 * SEC) == UVC_RECOVERY_RESTART);
	uvc_recovery_restart_finished(&r, 50 * SEC);
	assert(uvc_recovery_poll(&r, 58 * SEC) == UVC_RECOVERY_RESET);
	assert(!uvc_recovery_allow_reset(&limit, 58 * SEC));
	assert(!uvc_recovery_allow_reset(&limit, 70 * SEC));
	assert(uvc_recovery_allow_reset(&limit, 71 * SEC));
	/* Valid frames restart both the deadline and recovery stage. */
	uvc_recovery_frame(&r, 80 * SEC);
	assert(uvc_recovery_poll(&r, 87 * SEC) == UVC_RECOVERY_WAIT);
	assert(uvc_recovery_poll(&r, 88 * SEC) == UVC_RECOVERY_RESTART);
	assert(uvc_recovery_poll(&r, 79 * SEC) == UVC_RECOVERY_WAIT);
	puts("uvc recovery tests passed");
}
