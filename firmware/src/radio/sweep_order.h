/**
 * Visit order for the sweep.
 *
 * Channels are visited with a rotating stride rather than left to right, so a
 * periodic emitter cannot stay in step with the sweep and hide from every pass.
 * The stride has to be coprime with the channel count, otherwise the walk
 * closes early and part of the band is never measured - which is why this lives
 * in its own file with a test.
 */
#ifndef PIXLA_SWEEP_ORDER_H
#define PIXLA_SWEEP_ORDER_H

#include <stdint.h>

uint16_t sweep_order_next_seed(uint16_t seed);

// A stride in 1..count-1 that is coprime with count (1 when count < 3)
uint8_t sweep_order_stride(uint8_t count, uint16_t seed);

#endif // PIXLA_SWEEP_ORDER_H
