/** Settings screen: pick a row, MID to edit it, LEFT/RIGHT to change it. */
#include <string.h>

#include "app_config.h"
#include "battery.h"
#include "ble_beacon.h"
#include "display.h"
#include "gfx.h"
#include "scanner.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"

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
    SET_INVERT,
#ifdef OLED_TYPE_SH1106
    SET_BURNIN,
    SET_SAVER,
#endif
    SET_SENTRY_DB,
    SET_SENTRY_MS,
    SET_SNIFFPHY,
    SET_SNIFFBITS,
    SET_BEACON,
    SET_BEACONINT,
    SET_COUNT
};

static const char *const settings_items[SET_COUNT] = {
    "Back",     "Contrast", "Backlight", "Band",     "Dwell",   "WF speed", "WF mode",
    "Auto floor", "Shuffle", "LED hunt", "Dim s",    "Sleep m", "Batt cal", "Invert",
#ifdef OLED_TYPE_SH1106
    "Px shift", "Saver m",
#endif
    "Sentry dB", "Sentry ms",
    "Sniff PHY", "Sniff bits", "Beacon", "Beacon int",
};

static uint8_t m_settings_sel;
static bool m_settings_edit;

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

    values[SET_INVERT] = g_settings.invert ? "ON" : "OFF";
#ifdef OLED_TYPE_SH1106
    values[SET_BURNIN] = g_settings.burnin ? "ON" : "OFF";
    if (g_settings.saver_min)
        values[SET_SAVER] = gfx_fmt_int(storage[SET_SAVER], g_settings.saver_min);
    else
        values[SET_SAVER] = "OFF";
#endif

    gfx_fmt_int(storage[SET_SENTRY_DB], g_settings.sentry_db);
    values[SET_SENTRY_DB] = storage[SET_SENTRY_DB];

    gfx_fmt_int(storage[SET_SENTRY_MS], g_settings.sentry_period * 100);
    values[SET_SENTRY_MS] = storage[SET_SENTRY_MS];

    values[SET_SNIFFPHY] = g_settings.sniff_rate == 1   ? "2M"
                           : g_settings.sniff_rate == 2 ? "1M"
                                                        : "AUTO";
    values[SET_SNIFFBITS] = g_settings.sniff_bits ? "LSB" : "MSB";
    values[SET_BEACON] = ble_beacon_name(g_settings.beacon_type);
    gfx_fmt_int(storage[SET_BEACONINT],
                (int)ble_beacon_intervals[g_settings.beacon_int < BLE_BEACON_INTV_COUNT
                                              ? g_settings.beacon_int
                                              : 0]);
    values[SET_BEACONINT] = storage[SET_BEACONINT];
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
        scr_scanner_apply_band();
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
    case SET_INVERT:
        g_settings.invert = !g_settings.invert;
        display_set_inverted(g_settings.invert != 0);
        break;
#ifdef OLED_TYPE_SH1106
    case SET_BURNIN:
        // The main loop moves the frame, or puts it back when this goes off
        g_settings.burnin = !g_settings.burnin;
        break;
    case SET_SAVER:
    {
        int v = g_settings.saver_min + dir;
        g_settings.saver_min = (uint8_t)(v < 0 ? 0 : (v > 60 ? 60 : v));
        break;
    }
#endif
    case SET_SENTRY_DB:
    {
        int v = g_settings.sentry_db + dir;
        g_settings.sentry_db = (uint8_t)(v < 6 ? 6 : (v > 40 ? 40 : v));
        break;
    }
    case SET_SENTRY_MS:
    {
        int v = g_settings.sentry_period + dir;
        g_settings.sentry_period = (uint8_t)(v < 1 ? 1 : (v > 50 ? 50 : v));
        break;
    }
    case SET_SNIFFPHY:
    {
        int v = g_settings.sniff_rate + dir;
        g_settings.sniff_rate = (uint8_t)(v < 0 ? 2 : (v > 2 ? 0 : v));
        break;
    }
    case SET_SNIFFBITS:
        g_settings.sniff_bits = !g_settings.sniff_bits;
        break;
    case SET_BEACON:
    {
        int v = g_settings.beacon_type + dir;
        g_settings.beacon_type =
            (uint8_t)(v < 0 ? BLE_BEACON_TYPE_COUNT - 1
                            : (v >= BLE_BEACON_TYPE_COUNT ? 0 : v));
        break;
    }
    case SET_BEACONINT:
    {
        int v = g_settings.beacon_int + dir;
        g_settings.beacon_int = (uint8_t)(v < 0 ? BLE_BEACON_INTV_COUNT - 1
                                                : (v >= BLE_BEACON_INTV_COUNT ? 0 : v));
        break;
    }
    default:
        return;
    }
    settings_mark_dirty();
}

static void settings_enter(void)
{
    m_settings_sel = 0;
    m_settings_edit = false;
}

// Saved however the screen is left: the Back row or a long LEFT
static void settings_leave(void)
{
    if (settings_dirty())
        settings_save();
}

static void settings_tick(uint32_t now)
{
    if (m_settings_edit)
    {
        if (app_left())
        {
            settings_adjust(-1);
            app_redraw();
        }
        if (app_right())
        {
            settings_adjust(1);
            app_redraw();
        }
        if (app_ok())
        {
            m_settings_edit = false;
            app_redraw();
        }
    }
    else
    {
        if (app_left())
        {
            m_settings_sel = (uint8_t)((m_settings_sel + SET_COUNT - 1) % SET_COUNT);
            app_redraw();
        }
        if (app_right())
        {
            m_settings_sel = (uint8_t)((m_settings_sel + 1) % SET_COUNT);
            app_redraw();
        }
        if (app_ok())
        {
            if (m_settings_sel == SET_BACK)
            {
                app_back();
                return;
            }
            m_settings_edit = true;
            app_redraw();
        }
    }

    if (app_take_redraw())
    {
        static char storage[SET_COUNT][8];
        const char *values[SET_COUNT];
        settings_values(values, storage);
        ui_list(m_settings_edit ? "SET: EDIT" : "SETTINGS", settings_items, SET_COUNT,
                m_settings_sel, values);
    }
}

const app_screen_t scr_settings = {
    .name = "Settings",
    .group = APP_GROUP_SYSTEM,
    .enter = settings_enter,
    .tick = settings_tick,
    .leave = settings_leave,
};
