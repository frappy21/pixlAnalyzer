/**
 * Home screen (the scanner) plus the screens and menu actions that work on
 * the same sweep: Busiest, Meter, Overlay, Freeze, Set ref, Clear max.
 */
#include <string.h>

#include "app_config.h"
#include "buttons.h"
#include "channels.h"
#include "display.h"
#include "led.h"
#include "scanner.h"
#include "screens.h"
#include "settings.h"
#include "spectrum.h"
#include "systime.h"
#include "ui.h"

static scanner_view_t m_view = {
    .tool = TOOL_MARK,
    .marker_col = DISP_W / 2,
    .scroll_back = 0,
    .frozen = false,
    .plan = PLAN_WIFI,
    .sweeps_s = 0,
};

static uint32_t m_sweep_count;
static uint32_t m_sweep_window_ms;

// Meter trend ring
static uint8_t m_trend[DISP_W - 4];
static uint8_t m_trend_len;

static const uint16_t ble_adv_mhz[3] = {2402, 2426, 2480};

// The scanner sweeps far faster than the display can usefully show, so frames
// are capped at ~30 per second. Button actions still redraw at once.
#define SCANNER_FRAME_MS 33

// ---------------------------------------------------------------------------
// Shared with other screens
// ---------------------------------------------------------------------------

uint16_t scr_scanner_marker_mhz(void)
{
    return scanner_mhz(spectrum_col_to_chan(m_view.marker_col));
}

void scr_scanner_apply_band(void)
{
    switch (g_settings.band)
    {
    case BAND_FULL:
        scanner_set_span(2400, 2500);
        break;
    case BAND_EXTENDED:
        scanner_set_span(2360, 2500);
        break;
    case BAND_BLE_ADV:
        scanner_set_channels(ble_adv_mhz, 3);
        break;
    case BAND_ISM:
    default:
        scanner_set_span(2400, 2483);
        break;
    }
    spectrum_reset();
}

// ---------------------------------------------------------------------------
// Scanner screen
// ---------------------------------------------------------------------------

static void tool_adjust(int direction)
{
    switch (m_view.tool)
    {
    case TOOL_MARK:
        m_view.marker_col += direction;
        if (m_view.marker_col < 0)
            m_view.marker_col = 0;
        if (m_view.marker_col >= DISP_W)
            m_view.marker_col = DISP_W - 1;
        break;

    case TOOL_SPAN:
    {
        // Zoom around the marker. This buys pixels per MHz and sweeps per
        // second, never finer resolution: the radio tunes in whole MHz.
        uint16_t lo = scanner_span_start();
        uint16_t hi = scanner_span_end();
        uint16_t center = scr_scanner_marker_mhz();
        uint16_t half = (hi - lo) / 2;

        if (direction > 0)
            half = half > 4 ? half * 2 / 3 : half;
        else
            half = half * 3 / 2;

        if (half < 4)
            half = 4;
        if (half > 70)
            half = 70;

        uint16_t new_lo = center > half ? center - half : 2360;
        uint16_t new_hi = center + half;
        scanner_set_span(new_lo, new_hi);
        spectrum_reset();
        break;
    }

    case TOOL_WFALL:
    {
        int decim = g_settings.wf_decim + direction;
        if (decim < 1)
            decim = 1;
        if (decim > 32)
            decim = 32;
        g_settings.wf_decim = (uint8_t)decim;
        settings_mark_dirty();
        break;
    }

    case TOOL_SCROLL:
    {
        int back = (int)m_view.scroll_back - direction * 4;
        if (back < 0)
            back = 0;
        uint16_t rows = spectrum_history_rows();
        if (rows > WATERFALL_ROWS && back > rows - WATERFALL_ROWS)
            back = rows - WATERFALL_ROWS;
        m_view.scroll_back = (uint16_t)back;
        m_view.frozen = (m_view.scroll_back != 0);
        break;
    }

    default:
        break;
    }
}

static void scanner_enter(void)
{
    m_sweep_window_ms = systime_ms();
}

static void scanner_tick(uint32_t now)
{
    static uint32_t last_draw_ms;

    if (!m_view.frozen)
    {
        scanner_sweep();
        spectrum_update(now);

        m_sweep_count++;
        if (now - m_sweep_window_ms >= 1000)
        {
            m_view.sweeps_s = m_sweep_count;
            m_sweep_count = 0;
            m_sweep_window_ms = now;
        }
        if (systime_ms() - last_draw_ms >= SCANNER_FRAME_MS)
            app_redraw();
    }

    // Clicks only: the long presses switch screens and open the menu
    if (app_left())
    {
        tool_adjust(-1);
        app_redraw();
    }
    if (app_right())
    {
        tool_adjust(1);
        app_redraw();
    }

    // A click cycles the tool
    if (app_ok())
    {
        m_view.tool = (uint8_t)((m_view.tool + 1) % TOOL_COUNT);
        if (m_view.tool != TOOL_SCROLL)
        {
            m_view.scroll_back = 0;
            m_view.frozen = false;
        }
        app_redraw();
    }

    if (app_take_redraw())
    {
        ui_scanner(&m_view);
        last_draw_ms = systime_ms();
    }
    else if (m_view.frozen)
    {
        // Scrolled back and nothing changed: no sweep runs, so do not spin
        systime_idle(20);
    }
}

const app_screen_t scr_scanner = {
    .name = "Spectrum",
    .group = APP_GROUP_SPECTRUM,
    .enter = scanner_enter,
    .tick = scanner_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Busiest channels
// ---------------------------------------------------------------------------

static void top_tick(uint32_t now)
{
    static uint32_t last_ms;

    // Keep sweeping so the occupancy figures stay live
    scanner_sweep();
    spectrum_update(now);

    if (app_take_redraw() || now - last_ms > 400)
    {
        ui_top_channels();
        last_ms = now;
    }

    if (app_any())
        app_back();
}

const app_screen_t scr_top = {
    .name = "WiFi",
    .group = APP_GROUP_SPECTRUM,
    .tick = top_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Meter
// ---------------------------------------------------------------------------

static void meter_enter(void)
{
    m_trend_len = 0;
    m_view.tool = TOOL_MARK; // left/right tune the meter, nothing else
}

static void meter_leave(void)
{
    led_off();
}

static void meter_tick(uint32_t now)
{
    uint16_t mhz = scr_scanner_marker_mhz();
    uint8_t rssi = scanner_measure(mhz, 64);

    uint8_t floor_v = NOISE_FLOOR_INIT;
    uint8_t db = (rssi != RSSI_INVALID && rssi < floor_v) ? floor_v - rssi : 0;
    if (db > SPECTRUM_RANGE_DB)
        db = SPECTRUM_RANGE_DB;

    if (m_trend_len < sizeof(m_trend))
    {
        m_trend[m_trend_len++] = db;
    }
    else
    {
        memmove(m_trend, m_trend + 1, sizeof(m_trend) - 1);
        m_trend[sizeof(m_trend) - 1] = db;
    }

    if (g_settings.led_hunt)
        led_set_rate(db == 0 ? 0 : (uint8_t)(1 + (db * 14) / SPECTRUM_RANGE_DB));
    led_update(now);

    // Redrawn on every pass anyway
    (void)app_take_redraw();
    ui_meter(mhz, rssi, db, m_trend, m_trend_len);

    if (app_left())
        tool_adjust(-1);
    if (app_right())
        tool_adjust(1);
    if (app_ok())
        app_back();
}

const app_screen_t scr_meter = {
    .name = "Meter",
    .group = APP_GROUP_SPECTRUM,
    .enter = meter_enter,
    .tick = meter_tick,
    .leave = meter_leave,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Menu actions on the scanner view
// ---------------------------------------------------------------------------

static void overlay_action(void)
{
    m_view.plan = (uint8_t)((m_view.plan + 1) % PLAN_COUNT);
}

static const char *overlay_value(void)
{
    return channels_plan_name(m_view.plan);
}

const app_screen_t act_overlay = {
    .name = "Overlay",
    .group = APP_GROUP_SPECTRUM,
    .action = overlay_action,
    .value = overlay_value,
};

static void freeze_action(void)
{
    m_view.frozen = !m_view.frozen;
    if (!m_view.frozen)
        m_view.scroll_back = 0;
}

static const char *freeze_value(void)
{
    return m_view.frozen ? "ON" : "OFF";
}

const app_screen_t act_freeze = {
    .name = "Freeze",
    .group = APP_GROUP_SPECTRUM,
    .action = freeze_action,
    .value = freeze_value,
};

static void set_ref_action(void)
{
    if (spectrum_has_ref())
        spectrum_clear_ref();
    else
        spectrum_snapshot_ref();
}

static const char *set_ref_value(void)
{
    return spectrum_has_ref() ? "SET" : "-";
}

const app_screen_t act_set_ref = {
    .name = "Set ref",
    .group = APP_GROUP_SPECTRUM,
    .action = set_ref_action,
    .value = set_ref_value,
};

static void clear_max_action(void)
{
    spectrum_clear_max();
    app_open(&scr_scanner); // straight to Spectrum to watch it build up again
}

const app_screen_t act_clear_max = {
    .name = "Clear max",
    .group = APP_GROUP_SPECTRUM,
    .action = clear_max_action,
};
