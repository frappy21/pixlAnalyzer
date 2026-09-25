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

// ---------------------------------------------------------------------------
// Crash catcher
//
// HardFault and MemManage store the fault registers in RAM the startup code
// does not clear (.noinit, ld/loader.ld) and reset. The record survives soft
// resets, watchdog resets and further crashes, not a power cycle or SYSTEM
// OFF. The next boot shows it once; Info shows it for as long as it exists.
// ---------------------------------------------------------------------------

typedef struct
{
    uint32_t pc;    // faulting instruction, 0 when the frame could not be read
    uint32_t lr;    // return address of the faulting function
    uint32_t xpsr;
    uint32_t cfsr;  // configurable fault status (MMFSR, BFSR, UFSR)
    uint32_t hfsr;
    uint32_t bfar;  // bus fault address, valid when CFSR.BFARVALID
    uint32_t mmfar; // MPU fault address, valid when CFSR.MMARVALID
    uint32_t sp;    // stack pointer at the fault (the exception frame)
    uint8_t vector; // 3 HardFault, 4 MemManage
    bool overflow;  // the stack ran into the guard region
    uint16_t count; // crashes since the record was created
} power_crash_t;

// The last crash, false when there is none
bool power_crash_get(power_crash_t *out);

// A crash this boot has not shown yet, and marking it shown
bool power_crash_fresh(void);
void power_crash_mark_shown(void);

// Deliberate faults for checking the catcher on hardware, never return:
// an undefined instruction, and unbounded recursion into the stack guard
void power_crash_test(bool stack_overflow);

// ---------------------------------------------------------------------------
// Stack and RAM
//
// power_init() paints the unused stack and puts a no-access MPU region over
// its lowest POWER_STACK_GUARD bytes, so an overflow faults into the crash
// catcher instead of running on into the variables below the stack.
// ---------------------------------------------------------------------------

#define POWER_STACK_GUARD 32

uint32_t power_stack_size(void);   // usable stack, guard excluded
uint32_t power_stack_unused(void); // never touched since boot (high-water mark)
uint32_t power_ram_gap(void);      // RAM between the static variables and the stack

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

// Stops the 64MHz crystal between radio bursts (sentry mode). The radio must
// be disabled first; radio_hfxo_start() restarts it.
void power_hfxo_release(void);

// Die temperature from the TEMP peripheral, in 0.25 degree C steps
int32_t power_temperature_q2(void);

// Runtime across resets: call with the uptime of this boot once a second;
// power_runtime_s() adds the time of the boots before it since the last
// power cycle (a crash or a watchdog reset does not restart it)
void power_runtime_update(uint32_t uptime_s);
uint32_t power_runtime_s(void);

#endif // PIXLA_POWER_H
