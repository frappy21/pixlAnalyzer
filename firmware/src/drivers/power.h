/**
 * Reset, sleep, brownout protection and the handover to the DFU bootloader.
 */
#ifndef PIXLA_POWER_H
#define PIXLA_POWER_H

#include <stdbool.h>
#include <stdint.h>

// Points the vector table at the application, enables brownout protection and
// records why we booted. Call first, before anything else.
void power_init(void);

// True when this boot came out of our own SYSTEM OFF sleep (button wake),
// which is the only case where the "hold to start" gate makes sense.
bool power_woke_from_sleep(void);

// Raw RESETREAS latched at boot, and a short name for it. This is how we tell
// "the firmware went to sleep" apart from "the supply collapsed".
uint32_t power_reset_reason(void);
const char *power_reset_reason_name(void);

// Watchdog: once started it cannot be stopped, so feed it from every loop
// that can run longer than a few milliseconds.
void power_watchdog_start(void);
void power_watchdog_feed(void);

// True once the supply has dropped below the brownout threshold
bool power_brownout(void);

// Blanks the display, stops the radio and enters SYSTEM OFF.
// Wake up source is the middle button, never returns.
void power_enter_deep_sleep(void);

// Sets GPREGRET and resets so the bootloader stays in DFU mode, never returns.
void power_enter_dfu(void);

#endif // PIXLA_POWER_H
