/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../encoder-health.h"
#include <assert.h>

#define S 1000000000ULL
#define LIMIT (5 * S)
int main(void)
{
	/* Startup may take up to the timeout; progress resets the deadline. */
	struct mc_flow f = {0};
	mc_flow_submit(&f, S);
	assert(!mc_flow_stalled(&f, 6 * S, LIMIT));
	mc_flow_output(&f, 6 * S);
	assert(!mc_flow_stalled(&f, 20 * S, LIMIT));

	/* Continuing input and output, including multiple frames in flight. */
	mc_flow_submit(&f, 21 * S);
	mc_flow_submit(&f, 22 * S);
	mc_flow_output(&f, 23 * S);
	mc_flow_submit(&f, 27 * S);
	assert(!mc_flow_stalled(&f, 28 * S, LIMIT));
	mc_flow_output(&f, 28 * S);
	assert(!mc_flow_stalled(&f, 33 * S, LIMIT));
	mc_flow_output(&f, 33 * S);

	/* Idle pause starts a new deadline on resumed input. */
	assert(!mc_flow_stalled(&f, 100 * S, LIMIT));
	mc_flow_submit(&f, 100 * S);
	assert(!mc_flow_stalled(&f, 104 * S, LIMIT));
	assert(mc_flow_stalled(&f, 106 * S, LIMIT));
	mc_flow_output(&f, 106 * S);

	/* Pause while work remains pending does not erase its age. */
	mc_flow_submit(&f, 110 * S);
	mc_flow_submit(&f, 120 * S);
	assert(mc_flow_stalled(&f, 120 * S, LIMIT));

	/* An output may arrive before its submit is recorded under the mutex. */
	struct mc_flow race = {0};
	mc_flow_output(&race, 200 * S);
	assert(!mc_flow_stalled(&race, 199 * S, LIMIT));
	mc_flow_submit(&race, 199 * S);
	assert(!mc_flow_stalled(&race, 201 * S, LIMIT));
	mc_flow_submit(&race, 202 * S);
	assert(!mc_flow_stalled(&race, 207 * S, LIMIT));
	assert(mc_flow_stalled(&race, 208 * S, LIMIT));

	/* An older sampled time must not wrap into a false stall. */
	struct mc_flow snapshot = {0};
	mc_flow_submit(&snapshot, 300 * S);
	mc_flow_submit(&snapshot, 301 * S);
	mc_flow_output(&snapshot, 302 * S);
	assert(!mc_flow_stalled(&snapshot, 301 * S, LIMIT));
	assert(!mc_flow_stalled(&snapshot, 307 * S, LIMIT));
	assert(mc_flow_stalled(&snapshot, 308 * S, LIMIT));

	/* A full allowed pipeline is healthy, and normal output keeps it bounded. */
	struct mc_flow backlog = {0};
	for (unsigned i = 0; i < 120; ++i) mc_flow_submit(&backlog, i + 1);
	assert(!mc_flow_backlogged(&backlog, 120));
	for (unsigned i = 0; i < 600; ++i) {
		mc_flow_output(&backlog, 121 + i);
		mc_flow_submit(&backlog, 121 + i);
		assert(!mc_flow_backlogged(&backlog, 120));
	}
	/* Sustained 60-in/30-out crosses the pending limit despite progress. */
	for (unsigned i = 0; i < 2; ++i) mc_flow_submit(&backlog, 800 + i);
	mc_flow_output(&backlog, 802);
	assert(mc_flow_backlogged(&backlog, 120));
	mc_flow_output(&backlog, 803);
	assert(!mc_flow_backlogged(&backlog, 120));
	/* An output observed first must not wrap unsigned pending counts. */
	struct mc_flow ahead = {0};
	mc_flow_output(&ahead, 1);
	assert(!mc_flow_backlogged(&ahead, 0));
	mc_flow_submit(&ahead, 2);
	assert(!mc_flow_backlogged(&ahead, 0));
	mc_flow_submit(&ahead, 3);
	assert(mc_flow_backlogged(&ahead, 0));
	/* A real input pause may leave hardware pipeline frames pending. */
	struct mc_flow paused = {0};
	mc_flow_input(&paused, S, 3 * S);
	mc_flow_input(&paused, 2 * S, 3 * S);
	mc_flow_output(&paused, 3 * S);
	mc_flow_input(&paused, 100 * S, 3 * S);
	assert(paused.submitted == 3 && paused.output == 1);
	assert(!mc_flow_stalled(&paused, 100 * S, LIMIT));
	assert(!mc_flow_stalled(&paused, 105 * S, LIMIT));
	assert(mc_flow_stalled(&paused, 106 * S, LIMIT));
	assert(mc_flow_backlogged(&paused, 1));
	return 0;
}
