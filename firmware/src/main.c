/**
 * Pixl.js 2.4GHz Spectrum Analyzer - application entry point.
 *
 * Hardware: nRF52832, ST7565/ST7567 LCD or SH1106 OLED, bare metal (no SoftDevice).
 * The main loop ticks the screen on top of the navigation stack (src/app/app.h,
 * one scr_*.c file per screen) and runs the housekeeping. No input handling
 * blocks, and no screen redraws unless something actually changed.
 */
#include <stdbool.h>
#include <stdint.h>

#include "nrf_delay.h"

#include "app.h"
#include "app_config.h"
#include "battery.h"
#include "ble_scan.h"
#include "board_config.h"
#include "buttons.h"
#include "display.h"
#include "esb_scan.h"
#include "flash_ext.h"
#include "gfx.h"
#include "led.h"
#include "power.h"
#include "scanner.h"
#include "scr_system.h"
#include "screens.h"
#include "settings.h"
#include "systime.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_sys.h"

// OLED burn-in protection: the frame moves one pixel, around a 2x2 square,
// this often
#define BURNIN_SHIFT_MS 60000

// A crash report at boot waits this long for a button before going on
#define CRASH_REPORT_MS 30000

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void apply_settings(void)
{
    display_set_contrast(g_settings.contrast);
    display_set_backlight(g_settings.backlight);
    display_set_inverted(g_settings.invert != 0);
    scanner_set_dwell(g_settings.dwell);
    scanner_set_shuffle(g_settings.shuffle != 0);
    battery_set_calibration(g_settings.bat_cal);
}

// Sleeping is always deliberate: either the user asked, or the inactivity
// timer ran out. The reason goes on screen so an unexpected shutdown is never
// a mystery.
void go_to_sleep(const char *reason)
{
    if (settings_dirty())
        settings_save();
    ui_message("GOODBYE", reason, 800);
    power_enter_deep_sleep();
}

void go_to_dfu(uint32_t hold_ms)
{
    if (settings_dirty())
        settings_save();
    ui_message("BOOTLOADER", 0, hold_ms);
    power_enter_dfu();
}

// ---------------------------------------------------------------------------
// OLED burn-in protection
// ---------------------------------------------------------------------------

#ifdef OLED_TYPE_SH1106

static bool m_saver_on;

// Screensaver: the panel goes off and the first press only wakes it
static void saver_set(bool on)
{
    if (on == m_saver_on)
        return;
    m_saver_on = on;
    display_set_panel_off(on);
}

static void burnin_update(uint32_t now)
{
    static uint32_t last_ms;
    static uint8_t step; // 0..3 around the square, 0 is the unshifted frame

    if (!g_settings.burnin)
    {
        // Switched off: put the frame back where it belongs
        if (step)
        {
            step = 0;
            display_set_shift(0, 0);
            app_redraw();
        }
        return;
    }

    if (now - last_ms >= BURNIN_SHIFT_MS)
    {
        last_ms = now;
        step = (uint8_t)((step + 1) & 3);
        display_set_shift(step == 1 || step == 2, step >= 2);
        app_redraw(); // static screens only flush on a redraw
    }
}

#endif

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------

static void housekeeping(uint32_t now)
{
    static uint32_t last_battery_ms;
    static uint32_t last_runtime_ms;
    static bool warned_low;

    power_watchdog_feed();

    if (now - last_runtime_ms >= 1000)
    {
        power_runtime_update(now / 1000u);
        last_runtime_ms = now;
    }

#ifdef OLED_TYPE_SH1106
    burnin_update(now);
#endif

    if (now - last_battery_ms > 5000)
    {
        battery_update();
        last_battery_ms = now;

        // A low battery is shown, never acted on. The firmware measures the
        // cell under its own load and can read several hundred millivolts
        // low, so shutting down on that number would turn the device off
        // while the battery is still half full. POFCON is the real protection.
        if (g_battery.valid && g_battery.critical && !warned_low)
        {
            ui_low_battery();
            nrf_delay_ms(1500);
            warned_low = true;
            app_redraw();
        }
        else if (!g_battery.critical)
        {
            warned_low = false;
        }
    }

    // No dimming or sleeping while transmitting; sentry mode runs the
    // display itself and is meant to be left alone for hours
    if (tx_test_active() || scr_sentry_active())
        return;

    uint32_t idle = now - app_last_input_ms();
    if (idle > 86400000u)
        idle = 0; // clock glitch, never let it trigger a shutdown

    if (!app_dimmed() && g_settings.dim_s && idle > (uint32_t)g_settings.dim_s * 1000u)
        app_dim();

#ifdef OLED_TYPE_SH1106
    if (g_settings.saver_min && idle > (uint32_t)g_settings.saver_min * 60000u)
        saver_set(true);
#endif

    if (g_settings.sleep_min && idle > (uint32_t)g_settings.sleep_min * 60000u)
        go_to_sleep("NO INPUT");
}

// ---------------------------------------------------------------------------
// Recovery gesture
// ---------------------------------------------------------------------------

// Held sideways during the first second after start: go straight to the
// bootloader. The device has no exposed SWD pads, so a firmware that cannot be
// escaped from is a firmware that cannot be replaced. This path runs before
// anything else can go wrong.
static void check_dfu_gesture(void)
{
    for (int i = 0; i < 20; i++)
    {
        buttons_poll();
        if (!buttons_down(BTN_LEFT) && !buttons_down(BTN_RIGHT))
            return;

        display_clear();
        gfx_text(34, 18, "BOOTLOADER");
        gfx_text_micro(22, 30, "KEEP HOLDING TO ENTER DFU");
        gfx_box(14, 40, 100, 8, false, true);
        gfx_box(16, 42, (i * 96) / 19, 4, true, true);
        display_flush();
        nrf_delay_ms(50);
    }

    go_to_dfu(400);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    power_init();
    systime_init();
    buttons_init();
    led_init();

    settings_load();

    display_init();
    display_clear();
    display_flush();
    apply_settings();

    check_dfu_gesture();
    ui_power_on_gate();

    // The version goes onto the boot screen as an overlay
    display_set_overlay(ui_sys_boot_overlay);
    ui_boot_screen();
    display_set_overlay(0);

    // A crash before this boot is shown once, then kept under Info
    if (power_crash_fresh())
    {
        ui_sys_crash_report(CRASH_REPORT_MS);
        power_crash_mark_shown();
    }

    battery_init();
    battery_update();

    flash_ext_init(); // identify the chip for the info screen, never written to

    scanner_init();
    scr_scanner_apply_band();
    apply_settings();
    ble_scan_init();
    esb_scan_init();

    power_watchdog_start();

    app_init();

    while (1)
    {
        buttons_poll();
        uint32_t now = systime_ms();

#ifdef OLED_TYPE_SH1106
        // Any press ends the screensaver and is used up doing so
        if (m_saver_on && buttons_any_down())
        {
            saver_set(false);
            buttons_flush();
            app_note_input();
            app_redraw();
        }
#endif

        // Global long presses. On a main screen: long LEFT/RIGHT switch to
        // the previous/next main screen, long MID opens the menu. Anywhere
        // else a long LEFT closes whatever is open.
        if (app_at_home())
        {
            if (buttons_long(BTN_LEFT))
            {
                app_note_input();
                app_home_switch(-1);
            }
            else if (buttons_long(BTN_RIGHT))
            {
                app_note_input();
                app_home_switch(1);
            }
            else if (buttons_long(BTN_MID))
            {
                app_note_input();
                app_open(&scr_menu);
            }
        }
        else if (buttons_long(BTN_LEFT))
        {
            app_note_input();
            app_back();
        }

        const app_screen_t *screen = app_current();
        screen->tick(now);

        app_banner_update(now);
        housekeeping(now);

        // Screens that are not sweeping have nothing to do until a button
        // moves, so sleep the core instead of spinning at 64MHz. Read after
        // the tick: a screen that just opened a busy one must not delay it.
        if (!app_current()->busy)
            systime_idle(20);
    }
}
