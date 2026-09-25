/**
 * Info screen: three pages (system, power, last crash), LEFT/RIGHT to page
 * through them, long LEFT back. The crash page also holds the crash test:
 * a click picks the kind, a long MID fires it.
 */
#include "battery.h"
#include "power.h"
#include "screens.h"
#include "settings.h"
#include "spectrum.h"
#include "ui_info.h"

// Uptime and temperature keep moving, so the page refreshes on its own
#define INFO_REFRESH_MS 1000

static info_view_t m_view;
static uint32_t m_last_draw_ms;

static void info_enter(void)
{
    m_view.page = INFO_PAGE_SYSTEM;
    m_view.test_overflow = false;
}

static void info_tick(uint32_t now)
{
    if (app_left())
    {
        m_view.page = (uint8_t)((m_view.page + INFO_PAGE_COUNT - 1) % INFO_PAGE_COUNT);
        app_redraw();
    }
    if (app_right())
    {
        m_view.page = (uint8_t)((m_view.page + 1) % INFO_PAGE_COUNT);
        app_redraw();
    }

    if (m_view.page == INFO_PAGE_CRASH)
    {
        if (app_ok())
        {
            m_view.test_overflow = !m_view.test_overflow;
            app_redraw();
        }
        if (app_ok_long())
        {
            // The reset loses nothing the user changed
            if (settings_dirty())
                settings_save();
            power_crash_test(m_view.test_overflow);
        }
    }

    if (now - m_last_draw_ms >= INFO_REFRESH_MS)
        app_redraw();

    if (app_take_redraw())
    {
        if (m_view.page == INFO_PAGE_POWER)
            battery_update();
        m_view.uptime_s = (now - app_boot_ms()) / 1000u;
        m_view.sweeps = spectrum_sweeps();
        m_view.history_rows = spectrum_history_rows();
        ui_info(&m_view);
        m_last_draw_ms = now;
    }
}

const app_screen_t scr_info = {
    .name = "Info",
    .group = APP_GROUP_SYSTEM,
    .enter = info_enter,
    .tick = info_tick,
};
