/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Regression for the high-stability shedding gate in android-mediacodec.
 *
 * Motivated by real-device evidence (Xiaomi 25053RP5CC, c2.xring HEVC + H.264):
 *   - eglSwapBuffers submit cost stayed at 1.7-2.2ms for ~30s,
 *   - then jumped to 36-44ms and stayed there for the next 60s,
 *   - while the pending-input count sat at 0-2 the whole time,
 *   - and then the backlog jumped straight to the 300-frame hard limit,
 *     aborting the stream with "pending input exceeded its frame limit".
 *
 * The lesson encoded here: a gate that only watches the pending count can never
 * fire during that 60s window, because pending stays near zero while submits are
 * already ten times over budget. The gate must also key off measured submit cost.
 *
 * The policy under test is the REAL one: this file includes shed-policy.h from
 * the plugin, so the constants and the decision function are not mirrored here.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../shed-policy.h"

#define MAX_QUEUED 120
#define FPS 60
#define FRAME_NS (1000000000ULL / FPS)
#define BUDGET_US (2000000ULL / FPS)

/* Mirrors one encode() call plus the drain thread's bookkeeping, so the
 * invariants that actually broke on device (backlog reaching the hard limit,
 * output queue overflowing) are observable here. */
struct sim {
uint64_t submitted, output;
uint64_t queued; /* output packets waiting for the muxer */
int degraded;
uint64_t degraded_until_ns;
uint64_t dropped_load;
uint64_t max_pending;
uint64_t submit_budget_us;
uint64_t now;
bool shedding, ever_shed;
bool aborted;
};

static void sim_init(struct sim *s)
{
*s = (struct sim){0};
s->max_pending = (uint64_t)FPS * 5;
if (s->max_pending > MAX_QUEUED)
s->max_pending = MAX_QUEUED;
if (s->max_pending < (uint64_t)MC_SHED_SOFT_PENDING * 2)
s->max_pending = (uint64_t)MC_SHED_SOFT_PENDING * 2;
s->submit_budget_us = BUDGET_US;
}

/* Returns true if the frame was submitted, false if shed. */
static bool sim_frame(struct sim *s, uint64_t submit_us_this_frame)
{
s->now += FRAME_NS;
mc_shed_tick(&s->degraded, &s->degraded_until_ns, s->now);
const uint64_t pending = s->submitted > s->output ? s->submitted - s->output : 0;
const uint64_t soft = mc_shed_soft_limit(s->degraded);
const bool shed = mc_shed_decide(pending, s->degraded, soft);
s->shedding = shed;
if (shed) {
s->ever_shed = true;
s->dropped_load++;
/* The plugin still drains while shedding; a shed frame produces no
 * output of its own, so only the drain below can shrink `queued`. */
if (s->queued > MAX_QUEUED)
s->aborted = true;
return false;
}
mc_shed_note_submit(submit_us_this_frame, s->submit_budget_us, &s->degraded,
    &s->degraded_until_ns, s->now);
s->submitted++;
return true;
}

static void sim_output(struct sim *s, int frames)
{
s->output += (uint64_t)frames;
s->queued += (uint64_t)frames;
}

/* The muxer consumes packets; a stalled consumer is what overflowed the
 * 120-slot output queue in the field. */
static void sim_drain(struct sim *s, int frames)
{
if (s->queued >= (uint64_t)frames)
s->queued -= (uint64_t)frames;
else
s->queued = 0;
}

static uint64_t sim_pending(const struct sim *s)
{
return s->submitted > s->output ? s->submitted - s->output : 0;
}

/* --- Case 1: healthy 60fps. Nothing must be shed. --- */
static void test_healthy_no_shedding(void)
{
struct sim s;
sim_init(&s);
for (int i = 0; i < FPS * 60; i++) { /* 60 seconds */
if (sim_frame(&s, 1800))
sim_output(&s, 1);
sim_drain(&s, 1);
}
assert(s.dropped_load == 0);
assert(s.degraded == 0);
assert(!s.shedding);
assert(!s.aborted);
printf("PASS: healthy 60fps sheds nothing (%llu frames)\n",
       (unsigned long long)s.submitted);
}

/* --- Case 2: the real-device shape. ---
 * 30s healthy, then every submit costs 36ms while the codec still drains
 * ~43fps. The device showed pending sitting at 0-2 throughout, so the gate has
 * to fire off the submit cost alone; the backlog must stay bounded and the
 * output queue must never hit MAX_QUEUED. */
static void test_degraded_backlog_bounded(void)
{
struct sim s;
sim_init(&s);

for (int i = 0; i < FPS * 30; i++) {
if (sim_frame(&s, 1900))
sim_output(&s, 1);
sim_drain(&s, 1);
}
assert(s.dropped_load == 0);

int out_acc = 0;
for (int i = 0; i < FPS * 90; i++) {
(void)sim_frame(&s, 36000);
out_acc += 43;
if (out_acc >= FPS) {
sim_output(&s, out_acc / FPS);
out_acc %= FPS;
}
/* The muxer keeps up with what the codec produces. */
sim_drain(&s, 1);
assert(sim_pending(&s) < s.max_pending);
assert(s.queued <= MAX_QUEUED);
}

printf("PASS: degraded phase keeps backlog bounded (pending=%llu < hard=%llu, shed=%llu, out=%llu)\n",
       (unsigned long long)sim_pending(&s), (unsigned long long)s.max_pending,
       (unsigned long long)s.dropped_load, (unsigned long long)s.output);
assert(s.dropped_load > 0);
assert(s.ever_shed); /* pending may be 0 at this exact frame */
assert(s.output > 0);
assert(!s.aborted);
}

/* --- Case 3: recovery. After the codec speeds up again the gate must release
 * and stop shedding, otherwise a transient stall would cap quality forever. --- */
static void test_recovers(void)
{
struct sim s;
sim_init(&s);

for (int i = 0; i < FPS * 20; i++) {
(void)sim_frame(&s, 40000);
if (i % 2 == 0)
sim_output(&s, 1);
sim_drain(&s, 1);
}
assert(s.dropped_load > 0);
assert(s.ever_shed); /* pending may be 0 at this exact frame */

uint64_t before = s.submitted;
for (int i = 0; i < FPS * 10; i++) {
if (sim_frame(&s, 1500)) {
sim_output(&s, 1);
if (sim_pending(&s) > 2)
sim_output(&s, (int)(sim_pending(&s) - 2));
}
sim_drain(&s, 1);
}
printf("  recovery: degraded=%d shedding=%d submitted +%llu\n", s.degraded, (int)s.shedding,
       (unsigned long long)(s.submitted - before));
assert(s.degraded == 0);
assert(!s.shedding);
assert(s.submitted > before);
}

/* --- Case 4: the decay window decides how fast quality comes back. ---
 * A single over-budget submit opens the window; while it is open the gate can
 * shed freely. At expiry `degraded` steps to 1, not 0, for two reasons:
 *   - only 2 more over-budget submits re-collapse the soft limit, so a failed
 *     probe is caught within two frames instead of three;
 *   - the probing regime (soft limit back at the absolute depth, submit-cost
 *     arm inactive at degraded < 3) is entered deliberately, so the encoder
 *     actually gets to try a real submit instead of shedding forever. */
static void test_decay_steps_to_one(void)
{
int degraded = 3;
uint64_t until = 0;

mc_shed_note_submit(50000, BUDGET_US, &degraded, &until, 1000);
assert(degraded == MC_SHED_DEGRADED_MAX);
assert(until == 1000 + MC_SHED_RETRY_NS);
assert(mc_shed_soft_limit(degraded) == MC_SHED_MIN_PENDING);

mc_shed_tick(&degraded, &until, 1000 + MC_SHED_RETRY_NS);
assert(degraded == 1);
assert(until == 0);
/* Probing: full soft limit, submit-cost arm needs 3 strikes again. */
assert(mc_shed_soft_limit(degraded) == MC_SHED_SOFT_PENDING);
assert(!mc_shed_decide(1, degraded, mc_shed_soft_limit(degraded)));

/* Probe fails: one more over-budget submit re-collapses the limit. */
mc_shed_note_submit(50000, BUDGET_US, &degraded, &until, 3000);
assert(degraded == 2);
assert(mc_shed_soft_limit(degraded) == MC_SHED_MIN_PENDING);
mc_shed_note_submit(50000, BUDGET_US, &degraded, &until, 4000);
assert(degraded == 3);
assert(mc_shed_soft_limit(degraded) == MC_SHED_MIN_PENDING);

/* Probe succeeds: one fast submit clears the state entirely. */
degraded = 1;
until = 0;
mc_shed_note_submit(100, BUDGET_US, &degraded, &until, 5000);
assert(degraded == 0);
printf("PASS: decay steps 3 -> 1 -> (2 -> 3 | 0), never straight to 0\n");
}

/* --- Case 5: a single scheduler hiccup must not collapse the frame rate.
 * The submit-cost arm needs degraded >= 3, i.e. three consecutive over-budget
 * submits, before it sheds with pending == 1. --- */
static void test_needs_three_strikes(void)
{
int degraded = 0;
uint64_t until = 0;
uint64_t now = 0;

for (int i = 0; i < 3; i++) {
now += FRAME_NS;
mc_shed_note_submit(50000, BUDGET_US, &degraded, &until, now);
}
assert(degraded == 3);
assert(mc_shed_decide(1, degraded, mc_shed_soft_limit(degraded)));
/* Two strikes are not enough: one hiccup, then a healthy frame. */
mc_shed_note_submit(50000, BUDGET_US, &degraded, &until, now += FRAME_NS);
mc_shed_note_submit(100, BUDGET_US, &degraded, &until, now += FRAME_NS);
assert(degraded == 2);
assert(!mc_shed_decide(1, degraded, mc_shed_soft_limit(degraded)));
printf("PASS: submit-cost arm needs three consecutive over-budget submits\n");
}

int main(void)
{
test_healthy_no_shedding();
test_degraded_backlog_bounded();
test_recovers();
test_decay_steps_to_one();
test_needs_three_strikes();
puts("mediacodec stability gate tests passed");
return 0;
}