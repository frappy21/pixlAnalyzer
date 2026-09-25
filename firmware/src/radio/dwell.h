/**
 * Adaptive dwell policy: how many RSSI samples a channel visit takes.
 *
 * Quiet channels get half the base dwell, and the samples saved there go to
 * the channels that showed activity recently. The total per sweep never
 * exceeds count * base, so a sweep never takes longer than it does with the
 * fixed dwell: the sweep rate stays where it was (~118 per second on the ISM
 * band with the default 32 samples) or gets a little better.
 *
 * Pure arithmetic, no hardware, so the host tests can cover it.
 */
#ifndef PIXLA_DWELL_H
#define PIXLA_DWELL_H

#include <stdint.h>

// Fewest samples a quiet channel gets (never more than the base itself)
#define DWELL_QUIET_MIN 4

typedef struct
{
    uint8_t quiet;  // samples per visit of a quiet channel
    uint8_t active; // samples per visit of an active channel
} dwell_plan_t;

// base: the configured dwell, count: channels in the sweep, active: how many
// of them are currently marked active
dwell_plan_t dwell_plan(uint8_t base, uint8_t count, uint8_t active);

#endif // PIXLA_DWELL_H
