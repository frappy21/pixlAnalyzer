#include <string.h>

#include "nrf.h"
#include "nrf_gpio.h"

#include "app_config.h"
#include "board_config.h"
#include "display.h"
#include "flash_ext.h"
#include "led.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

// Vector table of this application, from gcc_startup_nrf52.S
extern uint32_t __isr_vector;

#define WDT_TIMEOUT_S 8
#define WDT_RELOAD_KEY 0x6E524635

// The wake button has to read released this long before SYSTEM OFF, so the
// release bounce is over too
#define SLEEP_RELEASE_MS 50

static bool m_woke_from_sleep;
static bool m_watchdog_running;
static uint32_t m_reset_reason;
static bool m_crash_boot; // this boot follows a crash nobody has seen yet

static void noinit_boot(void);
static void stack_guard_init(void);

void power_init(void)
{
    // The bootloader jumps straight into the application without a reset, so
    // the vector table still points at the SoftDevice area. Nothing here uses
    // interrupts yet, but anything that does would silently not work.
    SCB->VTOR = (uint32_t)&__isr_vector;

    // Why are we here? RESETREAS on its own is unreliable on this board, so
    // the sleep path leaves a cookie behind and we trust that instead.
    m_woke_from_sleep = (NRF_POWER->GPREGRET2 == WAKE_COOKIE);
    NRF_POWER->GPREGRET2 = 0;

    m_reset_reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFF;

    noinit_boot();
    stack_guard_init();

    // Brownout protection: a LiPo that sags under the radio load must not
    // corrupt flash. 2.7V leaves headroom above the 1.7V minimum.
    NRF_POWER->POFCON = (POWER_POFCON_THRESHOLD_V27 << POWER_POFCON_THRESHOLD_Pos) |
                        (POWER_POFCON_POF_Enabled << POWER_POFCON_POF_Pos);
    NRF_POWER->EVENTS_POFWARN = 0;

    // Code runs from flash with wait states; the cache makes the sweep and
    // render loops noticeably cheaper
    NRF_NVMC->ICACHECNF = NVMC_ICACHECNF_CACHEEN_Msk;

    // NOTE: the DC/DC regulator is deliberately NOT enabled. It needs an
    // external inductor that the pixl.js RevC bill of materials does not list,
    // and enabling DCDCEN without it browns the chip out.
}

bool power_woke_from_sleep(void)
{
    return m_woke_from_sleep;
}

uint32_t power_reset_reason(void)
{
    return m_reset_reason;
}

const char *power_reset_reason_name(void)
{
    // Checked in the order that matters: a crash, a brownout or a watchdog
    // reset means something is wrong, waking from SYSTEM OFF is normal. The
    // crash catcher resets with SYSRESETREQ, which alone would read SOFT.
    if (m_crash_boot)
        return "CRASH";
    if (m_reset_reason & POWER_RESETREAS_DOG_Msk)
        return "WATCHDOG";
    if (m_reset_reason & POWER_RESETREAS_OFF_Msk)
        return "WAKE";
    if (m_reset_reason & POWER_RESETREAS_RESETPIN_Msk)
        return "PIN";
    if (m_reset_reason & POWER_RESETREAS_SREQ_Msk)
        return "SOFT";
    if (m_reset_reason & POWER_RESETREAS_LOCKUP_Msk)
        return "LOCKUP";
    if (m_reset_reason == 0)
        return "POWER ON";
    return "OTHER";
}

bool power_brownout(void)
{
    return NRF_POWER->EVENTS_POFWARN != 0;
}

void power_watchdog_start(void)
{
    if (m_watchdog_running)
        return;

    // Pause while the CPU sleeps, so neither the idle WFE nor SYSTEM OFF can
    // ever be interrupted by a watchdog reset. It still guards running code.
    NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                      (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
    NRF_WDT->CRV = WDT_TIMEOUT_S * 32768 - 1;
    NRF_WDT->RREN = WDT_RREN_RR0_Msk;
    NRF_WDT->TASKS_START = 1;
    m_watchdog_running = true;
}

void power_watchdog_feed(void)
{
    if (m_watchdog_running)
        NRF_WDT->RR[0] = WDT_RELOAD_KEY;
}

// SYSTEM OFF with the wake pin already asserted wakes straight back up, so a
// Sleep chosen with a still held button would never sleep. Wait here, with
// the goodbye message still on screen, until the button is really released.
static void wait_wake_button_released(void)
{
    uint32_t released_at = systime_ms();

    while (1)
    {
        uint32_t now = systime_ms();
        if (nrf_gpio_pin_read(PIN_BTN_MID) == 0)
            released_at = now;
        else if (now - released_at >= SLEEP_RELEASE_MS)
            return;

        power_watchdog_feed();
        systime_idle(5);
    }
}

void power_enter_deep_sleep(void)
{
    wait_wake_button_released();

    // Deep power down for the SPI NOR while the shared bus is still up.
    // flash_ext_init() wakes it again on the next boot.
    flash_ext_sleep();

    display_clear();
    display_flush();
    display_uninit();
    led_off();

    scanner_stop();

    // Leave a cookie so the next boot knows the button that wakes us is still
    // held, and knows to run the hold-to-start gate
    NRF_POWER->GPREGRET2 = WAKE_COOKIE;

    nrf_gpio_cfg_sense_input(PIN_BTN_MID, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

    NRF_POWER->SYSTEMOFF = 1;

    // SYSTEM OFF can be refused while a debugger is attached; make that
    // visible instead of running on with the display torn down
    while (1)
        ;
}

void power_enter_dfu(void)
{
    // Signal to bootloader to enter DFU mode
    NRF_POWER->GPREGRET = BOOTLOADER_DFU_START;
    NVIC_SystemReset();
    while (1)
        ;
}

// ---------------------------------------------------------------------------
// RAM that survives a reset
// ---------------------------------------------------------------------------

#define NOINIT_MAGIC 0x4E4F494Eu // "NOIN"
#define CRASH_MAGIC 0x43524153u  // "CRAS"

#define CRASH_FLAG_OVERFLOW 0x01u
#define CRASH_FLAG_SHOWN 0x02u

typedef struct
{
    uint32_t magic; // NOINIT_MAGIC once initialised
    uint32_t check; // over every word below, catches random RAM after power up

    uint32_t crash_magic; // CRASH_MAGIC when the fields below hold a crash
    uint32_t pc, lr, xpsr, cfsr, hfsr, bfar, mmfar, sp;
    uint32_t vector;
    uint32_t flags;   // CRASH_FLAG_*
    uint32_t crashes; // crashes since the block was created

    uint32_t run_base_s; // runtime of the boots before this one
    uint32_t run_s;      // run_base_s plus this boot, updated once a second
} noinit_t;

static noinit_t m_noinit __attribute__((section(".noinit")));

static uint32_t noinit_sum(void)
{
    const uint32_t *w = &m_noinit.crash_magic;
    const uint32_t *end = (const uint32_t *)(&m_noinit + 1);
    uint32_t sum = 0x5EED1234u;
    while (w < end)
        sum = (sum << 5 | sum >> 27) ^ *w++;
    return sum;
}

static void noinit_seal(void)
{
    m_noinit.magic = NOINIT_MAGIC;
    m_noinit.check = noinit_sum();
}

static void noinit_boot(void)
{
    // RAM is not retained in SYSTEM OFF, and after a power cycle it holds
    // whatever it powered up with: only a block with an intact magic and
    // checksum is trusted. RESETREAS is not used for this, it is unreliable
    // on this board (see power_init).
    bool valid = m_noinit.magic == NOINIT_MAGIC && m_noinit.check == noinit_sum();
    if (!valid || m_woke_from_sleep || (m_reset_reason & POWER_RESETREAS_OFF_Msk))
        memset(&m_noinit, 0, sizeof(m_noinit));

    m_noinit.run_base_s = m_noinit.run_s;
    noinit_seal();

    m_crash_boot = power_crash_fresh();
}

void power_runtime_update(uint32_t uptime_s)
{
    m_noinit.run_s = m_noinit.run_base_s + uptime_s;
    noinit_seal();
}

uint32_t power_runtime_s(void)
{
    return m_noinit.run_s;
}

// ---------------------------------------------------------------------------
// Crash catcher
// ---------------------------------------------------------------------------

// From the linker: the stack occupies [__StackLimit, __StackTop), the static
// variables end at __bss_end__ (there is no heap)
extern uint32_t __StackLimit;
extern uint32_t __StackTop;
extern uint32_t __bss_end__;

#define RAM_START 0x20000000u
#define RAM_END 0x20010000u

bool power_crash_get(power_crash_t *out)
{
    if (m_noinit.crash_magic != CRASH_MAGIC)
        return false;

    out->pc = m_noinit.pc;
    out->lr = m_noinit.lr;
    out->xpsr = m_noinit.xpsr;
    out->cfsr = m_noinit.cfsr;
    out->hfsr = m_noinit.hfsr;
    out->bfar = m_noinit.bfar;
    out->mmfar = m_noinit.mmfar;
    out->sp = m_noinit.sp;
    out->vector = (uint8_t)m_noinit.vector;
    out->overflow = (m_noinit.flags & CRASH_FLAG_OVERFLOW) != 0;
    out->count = (uint16_t)(m_noinit.crashes > 0xFFFF ? 0xFFFF : m_noinit.crashes);
    return true;
}

bool power_crash_fresh(void)
{
    return m_noinit.crash_magic == CRASH_MAGIC && !(m_noinit.flags & CRASH_FLAG_SHOWN);
}

void power_crash_mark_shown(void)
{
    m_noinit.flags |= CRASH_FLAG_SHOWN;
    noinit_seal();
}

// Runs on a fresh stack (see the handler below), never returns. frame is the
// exception frame the core pushed: r0-r3, r12, lr, pc, xpsr.
void power_fault_record(const uint32_t *frame, uint32_t exc_return)
    __attribute__((used, noreturn));

void power_fault_record(const uint32_t *frame, uint32_t exc_return)
{
    uint32_t sp = (uint32_t)frame;
    uint32_t guard_end = (uint32_t)&__StackLimit + POWER_STACK_GUARD;
    uint32_t cfsr = SCB->CFSR;
    uint32_t mmfar = SCB->MMFAR;

    m_noinit.crash_magic = CRASH_MAGIC;
    m_noinit.cfsr = cfsr;
    m_noinit.hfsr = SCB->HFSR;
    m_noinit.bfar = SCB->BFAR;
    m_noinit.mmfar = mmfar;
    m_noinit.sp = sp;
    m_noinit.vector = __get_IPSR() & 0x1FF;

    // An overflow leaves the stack pointer in or right above the guard, and
    // the frame, if the core got to push one at all, is garbage. Reading it
    // from MemManage would fault again, so it is not read.
    bool overflow = sp < guard_end + 64 ||
                    ((cfsr & SCB_CFSR_MMARVALID_Msk) && mmfar >= (uint32_t)&__StackLimit &&
                     mmfar < guard_end);
    bool readable = !overflow && sp >= RAM_START && sp + 32 <= RAM_END && (sp & 3) == 0;

    m_noinit.pc = readable ? frame[6] : 0;
    m_noinit.lr = readable ? frame[5] : 0;
    m_noinit.xpsr = readable ? frame[7] : 0;
    m_noinit.flags = overflow ? CRASH_FLAG_OVERFLOW : 0;
    m_noinit.crashes++;
    noinit_seal();

    __DSB();
    NVIC_SystemReset();
    while (1)
        ;
}

// Both fault vectors land here. The stack pointer may sit inside the guard
// (a stack overflow), where even one push faults again, so the handler takes
// the frame address and moves MSP back to the top of the stack before any C
// code runs. Nothing returns from here, the record ends in a reset.
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile("tst lr, #4                \n"
                   "ite eq                    \n"
                   "mrseq r0, msp             \n"
                   "mrsne r0, psp             \n"
                   "mov r1, lr                \n"
                   "ldr r2, =__StackTop       \n"
                   "msr msp, r2               \n"
                   "b power_fault_record      \n"
                   ".ltorg                    \n");
}

// An MPU violation (the stack guard) arrives here, MemManage is enabled by
// stack_guard_init(). If stacking into the guard fails on the way in, the
// core escalates to HardFault from here, which it survives; the same failure
// on the way into HardFault itself would lock the core up.
__attribute__((naked)) void MemoryManagement_Handler(void)
{
    __asm volatile("b HardFault_Handler\n");
}

// Never inlined and never a tail call, so every level really takes stack.
// The limit is out of reach and only there so the recursion is not provably
// infinite to the compiler.
static volatile uint32_t m_recurse_limit = 0xFFFFFFFFu;

static uint32_t __attribute__((noinline)) recurse(uint32_t depth)
{
    volatile uint8_t pad[64];
    pad[0] = (uint8_t)depth;
    if (depth >= m_recurse_limit)
        return pad[0];
    return recurse(depth + 1) + pad[0];
}

void power_crash_test(bool stack_overflow)
{
    if (stack_overflow)
        recurse(0);

    __asm volatile("udf #0");
    while (1)
        ;
}

// ---------------------------------------------------------------------------
// Stack paint and guard
// ---------------------------------------------------------------------------

#define STACK_PAINT 0x57AC57ACu

static void stack_guard_init(void)
{
    uint32_t *p = &__StackLimit + POWER_STACK_GUARD / 4;
    uint32_t *end = (uint32_t *)(__get_MSP() - 64); // clear of this frame

    while (p < end)
        *p++ = STACK_PAINT;

    // The stack is aligned to its size at 0x2000F000, so the guard is
    // naturally aligned. Checked anyway: an MPU region base that is not a
    // multiple of the region size silently covers the wrong bytes.
    uint32_t base = (uint32_t)&__StackLimit;
    if (base & (POWER_STACK_GUARD - 1))
        return;

    // Region 0: no access, never executable. SIZE encodes 2^(SIZE+1) bytes.
    uint32_t size_field = 0;
    while ((2u << size_field) < POWER_STACK_GUARD)
        size_field++;

    MPU->RNR = 0;
    MPU->RBAR = base;
    MPU->RASR = MPU_RASR_XN_Msk | (0u << MPU_RASR_AP_Pos) | MPU_RASR_S_Msk | MPU_RASR_C_Msk |
                (size_field << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;

    // The default memory map stays in force everywhere else, and the MPU is
    // off inside HardFault (HFNMIENA clear) so that handler can always run
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    __DSB();
    __ISB();
}

uint32_t power_stack_size(void)
{
    return (uint32_t)((uint8_t *)&__StackTop - (uint8_t *)&__StackLimit) - POWER_STACK_GUARD;
}

uint32_t power_stack_unused(void)
{
    const uint32_t *p = &__StackLimit + POWER_STACK_GUARD / 4;
    const uint32_t *top = &__StackTop;
    uint32_t n = 0;

    while (p < top && *p == STACK_PAINT)
    {
        p++;
        n += 4;
    }
    return n;
}

uint32_t power_ram_gap(void)
{
    return (uint32_t)((uint8_t *)&__StackLimit - (uint8_t *)&__bss_end__);
}

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

void power_hfxo_release(void)
{
    // Back to the internal RC oscillator. The radio must be disabled; the
    // next radio_hfxo_start() brings the crystal back.
    NRF_CLOCK->TASKS_HFCLKSTOP = 1;
}

// ---------------------------------------------------------------------------
// Temperature
// ---------------------------------------------------------------------------

int32_t power_temperature_q2(void)
{
    // About 36us per conversion. The errata 66 calibration is loaded by
    // SystemInit.
    NRF_TEMP->EVENTS_DATARDY = 0;
    NRF_TEMP->TASKS_START = 1;
    for (uint32_t guard = 0; guard < 100000 && !NRF_TEMP->EVENTS_DATARDY; guard++)
        ;
    int32_t t = (int32_t)NRF_TEMP->TEMP;
    NRF_TEMP->TASKS_STOP = 1;
    NRF_TEMP->EVENTS_DATARDY = 0;
    return t;
}
