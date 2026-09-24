/**
 * Time base.
 *
 * RTC1 runs from the 32.768kHz crystal and provides the millisecond clock that
 * survives idle periods, TIMER0 provides a 1MHz counter for the microsecond
 * timestamps the burst analyzer needs.
 */
#ifndef PIXLA_SYSTIME_H
#define PIXLA_SYSTIME_H

#include <stdint.h>

void systime_init(void);

// Monotonic milliseconds since boot. Call at least once every 8 minutes so the
// 24 bit RTC counter never wraps twice between reads (the main loop does).
uint32_t systime_ms(void);

// Raw 1MHz counter, wraps every ~71 minutes. Use for short deltas only.
uint32_t systime_us(void);

// Sleep the core until the next RTC tick or a pending event, at most ms
// milliseconds. Used instead of busy waiting on menu screens.
void systime_idle(uint32_t ms);

#endif // PIXLA_SYSTIME_H
