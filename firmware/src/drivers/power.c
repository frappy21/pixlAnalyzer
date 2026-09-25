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
    // Checked in the order that matters: a brownout or a watchdog reset means
    // something is wrong, waking from SYSTEM OFF is normal
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
