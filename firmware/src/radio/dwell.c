#include "app_config.h"
#include "dwell.h"

dwell_plan_t dwell_plan(uint8_t base, uint8_t count, uint8_t active)
{
    if (base == 0)
        base = 1;
    if (active > count)
        active = count;

    dwell_plan_t plan = {.quiet = base, .active = base};

    // Nothing to shift the samples to, or nowhere to take them from
    if (active == 0 || active == count)
        return plan;

    uint8_t quiet = base / 2;
    if (quiet < DWELL_QUIET_MIN)
        quiet = base < DWELL_QUIET_MIN ? base : DWELL_QUIET_MIN;

    // The whole budget of the fixed dwell, minus what the quiet ones take.
    // quiet <= base, so this is never below base per active channel.
    uint32_t budget = (uint32_t)count * base;
    uint32_t rest = budget - (uint32_t)(count - active) * quiet;
    uint32_t per_active = rest / active;
    if (per_active > SCAN_DWELL_SAMPLES_MAX)
        per_active = SCAN_DWELL_SAMPLES_MAX;

    plan.quiet = quiet;
    plan.active = (uint8_t)per_active;
    return plan;
}
