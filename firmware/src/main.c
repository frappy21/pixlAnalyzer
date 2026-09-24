/**
 * Pixl.js 2.4GHz Spectrum Analyzer - application entry point.
 *
 * Hardware: nRF52832, ST7565/ST7567 LCD or SH1106 OLED, bare metal (no SoftDevice).
 * The main loop sweeps the band, renders a frame and turns button events into
 * screen changes. No input handling blocks, and no screen redraws unless
 * something actually changed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_delay.h"

#include "app_config.h"
#include "battery.h"
#include "ble_scan.h"
#include "board_config.h"
#include "buttons.h"
#include "channels.h"
#include "classify.h"
#include "display.h"
#include "esb_scan.h"
#include "flash_ext.h"
#include "gfx.h"
#include "led.h"
#include "power.h"
#include "scanner.h"
#include "settings.h"
#include "spectrum.h"
#include "systime.h"
#include "tx_test.h"
#include "ui.h"

typedef enum
{
    ST_SCANNER,
    ST_MENU,
    ST_SETTINGS,
    ST_INFO,
    ST_TOP,
    ST_METER,
    ST_IDENTIFY,
    ST_BLE,
    ST_BLE_DETAIL,
    ST_ESB,
    ST_TX_CONFIRM,
    ST_TX_ACTIVE,
} app_state_t;

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

static app_state_t m_state = ST_SCANNER;
static bool m_redraw = true;

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
static uint32_t m_last_input_ms;
static uint32_t m_boot_ms;
static bool m_dimmed;

static uint8_t m_menu_sel;
static uint8_t m_settings_sel;
static bool m_settings_edit;
static uint8_t m_ble_sel;
static uint16_t m_tx_mhz = 2440;
static uint8_t m_tx_power = TX_POWER_MIN;

// Identify capture buffer, 2KB of the 58KB we have spare
static burst_t m_bursts[256];
static verdict_t m_verdict;
static bool m_ble_confirmed;
static uint16_t m_ble_confirm_packets;

// Meter trend ring
static uint8_t m_trend[DISP_W - 4];
static uint8_t m_trend_len;

static const uint16_t ble_adv_mhz[3] = {2402, 2426, 2480};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void enter(app_state_t state)
{
    m_state = state;
    m_redraw = true;
}

static uint16_t marker_mhz(void)
{
    return scanner_mhz(spectrum_col_to_chan(m_view.marker_col));
}

static void apply_band(void)
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

static void apply_settings(void)
{
    display_set_contrast(g_settings.contrast);
    display_set_backlight(g_settings.backlight);
    scanner_set_dwell(g_settings.dwell);
    scanner_set_shuffle(g_settings.shuffle != 0);
    battery_set_calibration(g_settings.bat_cal);
}

static void note_input(void)
{
    m_last_input_ms = systime_ms();
    if (m_dimmed)
    {
        display_set_backlight(g_settings.backlight);
        m_dimmed = false;
    }
}

static bool any_button_event(void)
{
    return buttons_pressed(BTN_LEFT) || buttons_pressed(BTN_MID) || buttons_pressed(BTN_RIGHT);
}

// Sleeping is always deliberate: either the user asked, or the inactivity
// timer ran out. The reason goes on screen so an unexpected shutdown is never
// a mystery.
static void go_to_sleep(const char *reason)
{
    if (settings_dirty())
        settings_save();
    ui_message("GOODBYE", reason, 800);
    power_enter_deep_sleep();
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
        uint16_t center = marker_mhz();
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

static void state_scanner(uint32_t now)
{
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
        m_redraw = true;
    }

    if (buttons_repeat(BTN_LEFT))
    {
        tool_adjust(-1);
        note_input();
        m_redraw = true;
    }
    if (buttons_repeat(BTN_RIGHT))
    {
        tool_adjust(1);
        note_input();
        m_redraw = true;
    }

    // A long press cycles the tool, a short one opens the menu
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 600)
    {
        m_view.tool = (uint8_t)((m_view.tool + 1) % TOOL_COUNT);
        if (m_view.tool != TOOL_SCROLL)
        {
            m_view.scroll_back = 0;
            m_view.frozen = false;
        }
        buttons_flush();
        note_input();
        m_redraw = true;
    }
    else if (buttons_pressed(BTN_MID))
    {
        note_input();
        m_menu_sel = 0;
        enter(ST_MENU);
        return;
    }

    if (m_redraw)
    {
        ui_scanner(&m_view);
        m_redraw = false;
    }
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------

enum
{
    MENU_BACK = 0,
    MENU_IDENTIFY,
    MENU_BLE,
    MENU_ESB,
    MENU_TOP,
    MENU_METER,
    MENU_OVERLAY,
    MENU_FREEZE,
    MENU_REF,
    MENU_CLEARMAX,
    MENU_SETTINGS,
    MENU_SLEEP,
    MENU_TX,
    MENU_INFO,
    MENU_DFU,
    MENU_COUNT
};

static const char *const menu_items[MENU_COUNT] = {
    "Back",      "Identify",  "BLE scan",  "Mouse/Kbd", "Busiest",
    "Meter",     "Overlay",   "Freeze",    "Set ref",   "Clear max",
    "Settings",  "Sleep",     "TX test",   "Info",      "DFU",
};

static void menu_values(const char *values[MENU_COUNT])
{
    memset(values, 0, sizeof(const char *) * MENU_COUNT);
    values[MENU_OVERLAY] = channels_plan_name(m_view.plan);
    values[MENU_FREEZE] = m_view.frozen ? "ON" : "OFF";
    values[MENU_REF] = spectrum_has_ref() ? "SET" : "-";
}

static void state_menu(void)
{
    if (buttons_repeat(BTN_LEFT))
    {
        m_menu_sel = (uint8_t)((m_menu_sel + MENU_COUNT - 1) % MENU_COUNT);
        note_input();
        m_redraw = true;
    }
    if (buttons_repeat(BTN_RIGHT))
    {
        m_menu_sel = (uint8_t)((m_menu_sel + 1) % MENU_COUNT);
        note_input();
        m_redraw = true;
    }

    if (buttons_pressed(BTN_MID))
    {
        note_input();
        switch (m_menu_sel)
        {
        case MENU_BACK:
            enter(ST_SCANNER);
            return;

        case MENU_IDENTIFY:
            enter(ST_IDENTIFY);
            return;

        case MENU_BLE:
            ble_scan_reset();
            m_ble_sel = 0;
            enter(ST_BLE);
            return;

        case MENU_ESB:
            esb_scan_reset();
            enter(ST_ESB);
            return;

        case MENU_TOP:
            enter(ST_TOP);
            return;

        case MENU_METER:
            m_trend_len = 0;
            m_view.tool = TOOL_MARK; // left/right tune the meter, nothing else
            enter(ST_METER);
            return;

        case MENU_OVERLAY:
            m_view.plan = (uint8_t)((m_view.plan + 1) % PLAN_COUNT);
            break;

        case MENU_FREEZE:
            m_view.frozen = !m_view.frozen;
            if (!m_view.frozen)
                m_view.scroll_back = 0;
            break;

        case MENU_REF:
            if (spectrum_has_ref())
                spectrum_clear_ref();
            else
                spectrum_snapshot_ref();
            break;

        case MENU_CLEARMAX:
            spectrum_clear_max();
            enter(ST_SCANNER);
            return;

        case MENU_SETTINGS:
            m_settings_sel = 0;
            m_settings_edit = false;
            enter(ST_SETTINGS);
            return;

        case MENU_TX:
            m_tx_mhz = marker_mhz();
            enter(ST_TX_CONFIRM);
            return;

        case MENU_INFO:
            enter(ST_INFO);
            return;

        case MENU_SLEEP:
            go_to_sleep("MENU");
            return;

        case MENU_DFU:
            if (settings_dirty())
                settings_save();
            ui_message("BOOTLOADER", 0, 500);
            power_enter_dfu();
            return;

        default:
            break;
        }
        m_redraw = true;
    }

    if (m_redraw)
    {
        const char *values[MENU_COUNT];
        menu_values(values);
        ui_list("MENU", menu_items, MENU_COUNT, m_menu_sel, values);
        m_redraw = false;
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum
{
    SET_BACK = 0,
    SET_CONTRAST,
    SET_BACKLIGHT,
    SET_BAND,
    SET_DWELL,
    SET_WFSPEED,
    SET_WFMODE,
    SET_FLOOR,
    SET_SHUFFLE,
    SET_LED,
    SET_DIM,
    SET_SLEEP,
    SET_BATCAL,
    SET_COUNT
};

static const char *const settings_items[SET_COUNT] = {
    "Back",     "Contrast", "Backlight", "Band",     "Dwell",   "WF speed", "WF mode",
    "Auto floor", "Shuffle", "LED hunt", "Dim s",    "Sleep m", "Batt cal",
};

static const char *band_name(uint8_t band)
{
    switch (band)
    {
    case BAND_ISM:
        return "ISM";
    case BAND_FULL:
        return "FULL";
    case BAND_EXTENDED:
        return "EXT";
    case BAND_BLE_ADV:
        return "BLEADV";
    default:
        return "?";
    }
}

static void settings_values(const char *values[SET_COUNT], char storage[SET_COUNT][8])
{
    memset(values, 0, sizeof(const char *) * SET_COUNT);

    gfx_fmt_int(storage[SET_CONTRAST], g_settings.contrast);
    values[SET_CONTRAST] = storage[SET_CONTRAST];

    gfx_fmt_int(storage[SET_BACKLIGHT], g_settings.backlight);
    values[SET_BACKLIGHT] = storage[SET_BACKLIGHT];

    values[SET_BAND] = band_name(g_settings.band);

    gfx_fmt_int(storage[SET_DWELL], g_settings.dwell);
    values[SET_DWELL] = storage[SET_DWELL];

    gfx_fmt_int(storage[SET_WFSPEED], g_settings.wf_decim);
    values[SET_WFSPEED] = storage[SET_WFSPEED];

    values[SET_WFMODE] = (g_settings.wf_mode == WF_DITHER) ? "SHADES" : "1BIT";
    values[SET_FLOOR] = g_settings.auto_floor ? "AUTO" : "FIXED";
    values[SET_SHUFFLE] = g_settings.shuffle ? "ON" : "OFF";
    values[SET_LED] = g_settings.led_hunt ? "ON" : "OFF";

    gfx_fmt_int(storage[SET_DIM], g_settings.dim_s);
    values[SET_DIM] = storage[SET_DIM];

    gfx_fmt_int(storage[SET_SLEEP], g_settings.sleep_min);
    values[SET_SLEEP] = storage[SET_SLEEP];

    gfx_fmt_int(storage[SET_BATCAL], g_settings.bat_cal);
    values[SET_BATCAL] = storage[SET_BATCAL];
}

static void settings_adjust(int dir)
{
    switch (m_settings_sel)
    {
    case SET_CONTRAST:
    {
        int v = g_settings.contrast + dir;
        g_settings.contrast = (uint8_t)(v < 0 ? 0 : (v > 63 ? 63 : v));
        display_set_contrast(g_settings.contrast);
        break;
    }
    case SET_BACKLIGHT:
    {
        int v = g_settings.backlight + dir * 16;
        g_settings.backlight = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        display_set_backlight(g_settings.backlight);
        break;
    }
    case SET_BAND:
        g_settings.band = (uint8_t)((g_settings.band + BAND_COUNT + dir) % BAND_COUNT);
        apply_band();
        break;
    case SET_DWELL:
    {
        int v = g_settings.dwell + dir * 8;
        if (v < 4)
            v = 4;
        if (v > SCAN_DWELL_SAMPLES_MAX)
            v = SCAN_DWELL_SAMPLES_MAX;
        g_settings.dwell = (uint8_t)v;
        scanner_set_dwell(g_settings.dwell);
        break;
    }
    case SET_WFSPEED:
    {
        int v = g_settings.wf_decim + dir;
        g_settings.wf_decim = (uint8_t)(v < 1 ? 1 : (v > 32 ? 32 : v));
        break;
    }
    case SET_WFMODE:
        g_settings.wf_mode = (uint8_t)((g_settings.wf_mode + 1) % WF_MODE_COUNT);
        break;
    case SET_FLOOR:
        g_settings.auto_floor = !g_settings.auto_floor;
        break;
    case SET_SHUFFLE:
        g_settings.shuffle = !g_settings.shuffle;
        scanner_set_shuffle(g_settings.shuffle != 0);
        break;
    case SET_LED:
        g_settings.led_hunt = !g_settings.led_hunt;
        break;
    case SET_DIM:
    {
        int v = g_settings.dim_s + dir * 10;
        g_settings.dim_s = (uint8_t)(v < 0 ? 0 : (v > 250 ? 250 : v));
        break;
    }
    case SET_SLEEP:
    {
        int v = g_settings.sleep_min + dir;
        g_settings.sleep_min = (uint8_t)(v < 0 ? 0 : (v > 60 ? 60 : v));
        break;
    }
    case SET_BATCAL:
    {
        int v = g_settings.bat_cal + dir * 5;
        if (v < 800)
            v = 800;
        if (v > 1200)
            v = 1200;
        g_settings.bat_cal = (uint16_t)v;
        battery_set_calibration(g_settings.bat_cal);
        break;
    }
    default:
        return;
    }
    settings_mark_dirty();
}

static void state_settings(void)
{
    if (m_settings_edit)
    {
        if (buttons_repeat(BTN_LEFT))
        {
            settings_adjust(-1);
            note_input();
            m_redraw = true;
        }
        if (buttons_repeat(BTN_RIGHT))
        {
            settings_adjust(1);
            note_input();
            m_redraw = true;
        }
        if (buttons_pressed(BTN_MID))
        {
            m_settings_edit = false;
            note_input();
            m_redraw = true;
        }
    }
    else
    {
        if (buttons_repeat(BTN_LEFT))
        {
            m_settings_sel = (uint8_t)((m_settings_sel + SET_COUNT - 1) % SET_COUNT);
            note_input();
            m_redraw = true;
        }
        if (buttons_repeat(BTN_RIGHT))
        {
            m_settings_sel = (uint8_t)((m_settings_sel + 1) % SET_COUNT);
            note_input();
            m_redraw = true;
        }
        if (buttons_pressed(BTN_MID))
        {
            note_input();
            if (m_settings_sel == SET_BACK)
            {
                if (settings_dirty())
                    settings_save();
                enter(ST_MENU);
                return;
            }
            m_settings_edit = true;
            m_redraw = true;
        }
    }

    if (m_redraw)
    {
        static char storage[SET_COUNT][8];
        const char *values[SET_COUNT];
        settings_values(values, storage);
        ui_list(m_settings_edit ? "SET: EDIT" : "SETTINGS", settings_items, SET_COUNT,
                m_settings_sel, values);
        m_redraw = false;
    }
}

// ---------------------------------------------------------------------------
// Simple read only screens
// ---------------------------------------------------------------------------

static void state_info(uint32_t now)
{
    if (m_redraw)
    {
        battery_update();
        ui_info((now - m_boot_ms) / 1000u, spectrum_sweeps(), spectrum_history_rows());
        m_redraw = false;
    }

    if (any_button_event())
    {
        note_input();
        enter(ST_MENU);
    }
}

static void state_top(uint32_t now)
{
    static uint32_t last_ms;

    // Keep sweeping so the occupancy figures stay live
    scanner_sweep();
    spectrum_update(now);

    if (now - last_ms > 400 || m_redraw)
    {
        ui_top_channels();
        last_ms = now;
        m_redraw = false;
    }

    if (any_button_event())
    {
        note_input();
        enter(ST_MENU);
    }
}

static void state_meter(uint32_t now)
{
    uint16_t mhz = marker_mhz();
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

    ui_meter(mhz, rssi, db, m_trend, m_trend_len);

    if (buttons_repeat(BTN_LEFT))
    {
        tool_adjust(-1);
        note_input();
    }
    if (buttons_repeat(BTN_RIGHT))
    {
        tool_adjust(1);
        note_input();
    }
    if (buttons_pressed(BTN_MID))
    {
        note_input();
        led_off();
        enter(ST_MENU);
    }
}

// ---------------------------------------------------------------------------
// Identify: park on the marker, measure, then say what it looks like
// ---------------------------------------------------------------------------

static void run_identify(void)
{
    uint16_t mhz = marker_mhz();
    park_stats_t stats;
    uint16_t total = 0;

    // Four 250ms windows so the progress bar moves and the watchdog is fed
    park_stats_t acc;
    memset(&acc, 0, sizeof(acc));

    for (int i = 0; i < 4; i++)
    {
        ui_identify_progress(mhz, (uint8_t)(i * 25));
        memset(&stats, 0, sizeof(stats)); // a failed window must not add stale numbers
        uint16_t n = scanner_park(mhz, 250, &m_bursts[total],
                                  (uint16_t)(sizeof(m_bursts) / sizeof(m_bursts[0]) - total),
                                  &stats);
        // Burst timestamps restart with every window, so shift them
        for (uint16_t k = 0; k < n; k++)
            m_bursts[total + k].start_us += (uint32_t)i * 250000u;

        total = (uint16_t)(total + n);
        acc.window_us += stats.window_us;
        acc.on_us += stats.on_us;
        acc.bursts = (uint16_t)(acc.bursts + stats.bursts);
        acc.dropped = (uint16_t)(acc.dropped + stats.dropped);
        acc.floor_rssi = stats.floor_rssi;
        if (stats.peak_rssi < acc.peak_rssi || i == 0)
            acc.peak_rssi = stats.peak_rssi;

        power_watchdog_feed();
    }

    classify_run(mhz, m_bursts, total, &acc, &m_verdict);

    // On an advertising channel we do not have to guess: receive the packets
    m_ble_confirmed = false;
    m_ble_confirm_packets = 0;
    if (mhz == 2402 || mhz == 2426 || mhz == 2480)
    {
        ui_identify_progress(mhz, 90);
        ble_scan_reset();
        m_ble_confirm_packets = ble_scan_run(600);
        m_ble_confirmed = m_ble_confirm_packets > 0;
    }
}

static void state_identify(void)
{
    if (m_redraw)
    {
        run_identify();
        ui_identify(marker_mhz(), &m_verdict, m_ble_confirmed, m_ble_confirm_packets);
        m_redraw = false;
    }

    if (buttons_pressed(BTN_MID))
    {
        note_input();
        enter(ST_MENU);
        return;
    }
    if (buttons_pressed(BTN_LEFT) || buttons_pressed(BTN_RIGHT))
    {
        note_input();
        m_redraw = true; // measure again
    }
}

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

static void state_ble(uint32_t now)
{
    static uint32_t last_scan;

    if (now - last_scan > 200)
    {
        ble_scan_run(300);
        last_scan = now;
        m_redraw = true;
    }

    if (buttons_repeat(BTN_LEFT))
    {
        if (m_ble_sel)
            m_ble_sel--;
        note_input();
        m_redraw = true;
    }
    if (buttons_repeat(BTN_RIGHT))
    {
        if (m_ble_sel + 1 < ble_scan_count())
            m_ble_sel++;
        note_input();
        m_redraw = true;
    }
    if (buttons_pressed(BTN_MID))
    {
        note_input();
        if (ble_scan_count())
        {
            enter(ST_BLE_DETAIL);
            return;
        }
        enter(ST_MENU);
        return;
    }

    if (m_redraw)
    {
        ui_ble_list(m_ble_sel, ble_scan_packets());
        m_redraw = false;
    }
}

static void state_ble_detail(void)
{
    if (m_redraw)
    {
        uint8_t idx[BLE_MAX_DEVICES];
        uint8_t n = ble_scan_sorted(idx, BLE_MAX_DEVICES);
        const ble_dev_t *dev = (m_ble_sel < n) ? ble_scan_device(idx[m_ble_sel]) : 0;
        if (dev)
            ui_ble_detail(dev);
        m_redraw = false;
    }

    if (any_button_event())
    {
        note_input();
        enter(ST_BLE);
    }
}

// ---------------------------------------------------------------------------
// ShockBurst / nRF24 scan
// ---------------------------------------------------------------------------

static void state_esb(void)
{
    // One pass over the band the cheap radios use, in slices so the UI stays alive
    static uint16_t next_mhz = 2400;

    if (m_redraw)
    {
        ui_esb_list(esb_scan_total());
        m_redraw = false;
    }

    uint16_t end = next_mhz + 9;
    if (end > 2483)
        end = 2483;

    esb_scan_run(next_mhz, end, 30);
    next_mhz = (end >= 2483) ? 2400 : (uint16_t)(end + 1);
    m_redraw = true;

    if (any_button_event())
    {
        note_input();
        next_mhz = 2400;
        enter(ST_MENU);
    }
}

// ---------------------------------------------------------------------------
// Transmitter test
// ---------------------------------------------------------------------------

static void state_tx_confirm(void)
{
    if (buttons_repeat(BTN_LEFT))
    {
        m_tx_power = (uint8_t)((m_tx_power + TX_POWER_COUNT - 1) % TX_POWER_COUNT);
        note_input();
        m_redraw = true;
    }
    if (buttons_repeat(BTN_RIGHT))
    {
        m_tx_power = (uint8_t)((m_tx_power + 1) % TX_POWER_COUNT);
        note_input();
        m_redraw = true;
    }

    // Deliberate hold, so a stray press can never put a carrier on the air
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        buttons_flush();
        note_input();
        if (tx_test_start(m_tx_mhz, m_tx_power))
        {
            enter(ST_TX_ACTIVE);
            return;
        }
        ui_message("TX FAILED", 0, 800);
        enter(ST_MENU);
        return;
    }
    // A short press means "no thanks"
    if (buttons_pressed(BTN_MID))
    {
        note_input();
        enter(ST_MENU);
        return;
    }

    if (m_redraw)
    {
        ui_tx_confirm(m_tx_mhz, m_tx_power);
        m_redraw = false;
    }
}

static void state_tx_active(uint32_t now)
{
    tx_test_update(now);

    if (!tx_test_active())
    {
        ui_message("TX STOPPED", 0, 600);
        enter(ST_MENU);
        return;
    }

    if (any_button_event())
    {
        tx_test_stop();
        note_input();
        ui_message("TX STOPPED", 0, 600);
        enter(ST_MENU);
        return;
    }

    ui_tx_active(m_tx_mhz, m_tx_power, tx_test_remaining_ms(now));
}

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------

static void housekeeping(uint32_t now)
{
    static uint32_t last_battery_ms;
    static bool warned_low;

    power_watchdog_feed();

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
            m_redraw = true;
        }
        else if (!g_battery.critical)
        {
            warned_low = false;
        }
    }

    if (tx_test_active())
        return; // no dimming or sleeping while transmitting

    uint32_t idle = now - m_last_input_ms;
    if (idle > 86400000u)
        idle = 0; // clock glitch, never let it trigger a shutdown

    if (!m_dimmed && g_settings.dim_s && idle > (uint32_t)g_settings.dim_s * 1000u)
    {
        display_set_backlight(g_settings.backlight / 6);
        m_dimmed = true;
    }

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

    if (settings_dirty())
        settings_save();
    ui_message("BOOTLOADER", 0, 400);
    power_enter_dfu();
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
    ui_boot_screen();

    battery_init();
    battery_update();

    flash_ext_init(); // identify the chip for the info screen, never written to

    scanner_init();
    apply_band();
    apply_settings();
    ble_scan_init();
    esb_scan_init();

    power_watchdog_start();

    m_boot_ms = systime_ms();
    m_last_input_ms = m_boot_ms;
    m_sweep_window_ms = m_boot_ms;

    while (1)
    {
        buttons_poll();
        uint32_t now = systime_ms();

        switch (m_state)
        {
        case ST_SCANNER:
            state_scanner(now);
            break;
        case ST_MENU:
            state_menu();
            break;
        case ST_SETTINGS:
            state_settings();
            break;
        case ST_INFO:
            state_info(now);
            break;
        case ST_TOP:
            state_top(now);
            break;
        case ST_METER:
            state_meter(now);
            break;
        case ST_IDENTIFY:
            state_identify();
            break;
        case ST_BLE:
            state_ble(now);
            break;
        case ST_BLE_DETAIL:
            state_ble_detail();
            break;
        case ST_ESB:
            state_esb();
            break;
        case ST_TX_CONFIRM:
            state_tx_confirm();
            break;
        case ST_TX_ACTIVE:
            state_tx_active(now);
            break;
        }

        housekeeping(now);

        // Screens that are not sweeping have nothing to do until a button
        // moves, so sleep the core instead of spinning at 64MHz
        if (m_state != ST_SCANNER && m_state != ST_METER && m_state != ST_TOP &&
            m_state != ST_ESB && m_state != ST_IDENTIFY)
        {
            systime_idle(20);
        }
    }
}
