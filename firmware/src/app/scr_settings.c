/**
 * Settings screen, in pages: the first row cycles the page (Display, Radio,
 * Behaviour, Tools), the rows below edit as always - MID enters the edit,
 * LEFT/RIGHT change the value, MID leaves the edit. Saved on the way out.
 */
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

// ---------------------------------------------------------------------------
// Rows, one enum per page
// ---------------------------------------------------------------------------

typedef enum
{
    PG_DISPLAY = 0,
    PG_RADIO,
    PG_BEHAVIOR,
    PG_TOOLS,
    PG_COUNT
} settings_page_t;

static const char *const page_names[PG_COUNT] = {"DISPLAY", "RADIO", "BEHAVIOR", "TOOLS"};

// Per page row ids, SET_* indexes into flat storage
enum
{
    SET_PAGE = 0, // the page row, first on every page
    SET_BACK,     // last on every page

    SET_CONTRAST,
    SET_BACKLIGHT,
    SET_INVERT,
    SET_BURNIN,
    SET_SAVER,

    SET_BAND,
    SET_DWELL,
    SET_WFSPEED,
    SET_WFMODE,
    SET_FLOOR,
    SET_SHUFFLE,
    SET_SNIFFPHY,
    SET_SNIFFBITS,

    SET_LED,
    SET_DIM,
    SET_SLEEP,
    SET_HOME,

    SET_SENTRY_DB,
    SET_SENTRY_MS,
    SET_BEACON,
    SET_BEACONINT,
    SET_BATCAL,
    SET_NORLOG,

    SET_COUNT
};

// The rows of one page, terminated with SET_COUNT
typedef struct
{
    const char *const *labels;
    const uint8_t *rows;
    uint8_t count;
} settings_page_def_t;

static const char *c_l_display[] = {"Page", "Contrast", "Backlight", "Invert",
#ifdef OLED_TYPE_SH1106
                                    "Px shift", "Saver m",
#endif
                                    "Back"};
static const uint8_t c_r_display[] = {SET_PAGE, SET_CONTRAST, SET_BACKLIGHT, SET_INVERT,
#ifdef OLED_TYPE_SH1106
                                      SET_BURNIN, SET_SAVER,
#endif
                                      SET_BACK};

static const char *c_l_radio[] = {"Page", "Band",   "Dwell", "WF speed", "WF mode", "Auto floor",
                                  "Shuffle", "Sniff PHY", "Sniff bits", "Back"};
static const uint8_t c_r_radio[] = {SET_PAGE, SET_BAND,   SET_DWELL,  SET_WFSPEED,
                                    SET_WFMODE, SET_FLOOR, SET_SHUFFLE, SET_SNIFFPHY,
                                    SET_SNIFFBITS, SET_BACK};

static const char *c_l_behavior[] = {"Page", "LED hunt", "Dim s", "Sleep m", "Start screen", "Back"};
static const uint8_t c_r_behavior[] = {SET_PAGE, SET_LED, SET_DIM, SET_SLEEP, SET_HOME, SET_BACK};

static const char *c_l_tools[] = {"Page",     "Sentry dB", "Sentry ms", "Beacon",
                                  "Beacon int", "Batt cal",  "NOR log",   "Back"};
static const uint8_t c_r_tools[] = {SET_PAGE, SET_SENTRY_DB, SET_SENTRY_MS, SET_BEACON,
                                    SET_BEACONINT, SET_BATCAL, SET_NORLOG, SET_BACK};

#define PAGE_DEF(labels, rows) \
    {                          \
        labels, rows, (uint8_t)(sizeof(rows) / sizeof(rows[0])) \
    }

static const settings_page_def_t c_pages[PG_COUNT] = {
    PAGE_DEF(c_l_display, c_r_display),
    PAGE_DEF(c_l_radio, c_r_radio),
    PAGE_DEF(c_l_behavior, c_r_behavior),
    PAGE_DEF(c_l_tools, c_r_tools),
};

static uint8_t m_page;    // settings_page_t
static uint8_t m_sel;     // row selection within the page
static bool m_edit;

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

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

static void row_value(uint8_t row, char *storage, const char **out)
{
    *out = 0;

    switch (row)
    {
    case SET_PAGE:
        *out = page_names[m_page];
        break;
    case SET_CONTRAST:
        *out = gfx_fmt_int(storage, g_settings.contrast);
        break;
    case SET_BACKLIGHT:
        *out = gfx_fmt_int(storage, g_settings.backlight);
        break;
    case SET_INVERT:
        *out = g_settings.invert ? "ON" : "OFF";
        break;
#ifdef OLED_TYPE_SH1106
    case SET_BURNIN:
        *out = g_settings.burnin ? "ON" : "OFF";
        break;
    case SET_SAVER:
        *out = g_settings.saver_min ? gfx_fmt_int(storage, g_settings.saver_min) : "OFF";
        break;
#endif
    case SET_BAND:
        *out = band_name(g_settings.band);
        break;
    case SET_DWELL:
        *out = gfx_fmt_int(storage, g_settings.dwell);
        break;
    case SET_WFSPEED:
        *out = gfx_fmt_int(storage, g_settings.wf_decim);
        break;
    case SET_WFMODE:
        *out = (g_settings.wf_mode == WF_DITHER) ? "SHADES" : "1BIT";
        break;
    case SET_FLOOR:
        *out = g_settings.auto_floor ? "AUTO" : "FIXED";
        break;
    case SET_SHUFFLE:
        *out = g_settings.shuffle ? "ON" : "OFF";
        break;
    case SET_SNIFFPHY:
        *out = g_settings.sniff_rate == 1   ? "2M"
               : g_settings.sniff_rate == 2 ? "1M"
                                            : "AUTO";
        break;
    case SET_SNIFFBITS:
        *out = g_settings.sniff_bits ? "LSB" : "MSB";
        break;
    case SET_LED:
        *out = g_settings.led_hunt ? "ON" : "OFF";
        break;
    case SET_DIM:
        *out = gfx_fmt_int(storage, g_settings.dim_s);
        break;
    case SET_SLEEP:
        *out = gfx_fmt_int(storage, g_settings.sleep_min);
        break;
    case SET_HOME:
        // The carousel entry names, with the index shown for orientation
        if (g_settings.home_screen < g_app_home_count)
            *out = g_app_home_label[g_settings.home_screen];
        else
            *out = "SPECTRUM";
        break;
    case SET_SENTRY_DB:
        *out = gfx_fmt_int(storage, g_settings.sentry_db);
        break;
    case SET_SENTRY_MS:
        *out = gfx_fmt_int(storage, g_settings.sentry_period * 100);
        break;
    case SET_BEACON:
        *out = ble_beacon_name(g_settings.beacon_type);
        break;
    case SET_BEACONINT:
        *out = gfx_fmt_int(
            storage,
            (int)ble_beacon_intervals[g_settings.beacon_int < BLE_BEACON_INTV_COUNT
                                          ? g_settings.beacon_int
                                          : 0]);
        break;
    case SET_BATCAL:
        *out = gfx_fmt_int(storage, g_settings.bat_cal);
        break;
    case SET_NORLOG:
        *out = g_settings.log_consent ? "ON" : "OFF";
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Adjust
// ---------------------------------------------------------------------------

static void settings_adjust(int dir)
{
    uint8_t row = c_pages[m_page].rows[m_sel];

    switch (row)
    {
    case SET_PAGE:
        m_page = (uint8_t)((m_page + PG_COUNT + dir) % PG_COUNT);
        m_sel = 0;
        m_edit = false;
        break;
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
    case SET_INVERT:
        g_settings.invert = !g_settings.invert;
        display_set_inverted(g_settings.invert != 0);
        break;
#ifdef OLED_TYPE_SH1106
    case SET_BURNIN:
        g_settings.burnin = !g_settings.burnin;
        break;
    case SET_SAVER:
    {
        int v = g_settings.saver_min + dir;
        g_settings.saver_min = (uint8_t)(v < 0 ? 0 : (v > 60 ? 60 : v));
        break;
    }
#endif
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
    case SET_SNIFFPHY:
    {
        int v = g_settings.sniff_rate + dir;
        g_settings.sniff_rate = (uint8_t)(v < 0 ? 2 : (v > 2 ? 0 : v));
        break;
    }
    case SET_SNIFFBITS:
        g_settings.sniff_bits = !g_settings.sniff_bits;
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
    case SET_HOME:
    {
        int v = g_settings.home_screen + dir;
        g_settings.home_screen = (uint8_t)(v < 0 ? g_app_home_count - 1
                                                 : (v >= g_app_home_count ? 0 : v));
        break;
    }
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
    case SET_NORLOG:
        g_settings.log_consent = !g_settings.log_consent;
        break;
    default:
        return;
    }
    settings_mark_dirty();
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

static void settings_enter(void)
{
    m_page = 0;
    m_sel = 0;
    m_edit = false;
}

// Saved however the screen is left: the Back row or a long LEFT
static void settings_leave(void)
{
    if (settings_dirty())
        settings_save();
}

static void settings_tick(uint32_t now)
{
    (void)now;
    uint8_t rows = c_pages[m_page].count;
    uint8_t row = c_pages[m_page].rows[m_sel < rows ? m_sel : 0];

    if (m_edit)
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
            m_edit = false;
            app_redraw();
        }
    }
    else
    {
        if (app_left())
        {
            m_sel = (uint8_t)((m_sel + rows - 1) % rows);
            app_redraw();
        }
        if (app_right())
        {
            m_sel = (uint8_t)((m_sel + 1) % rows);
            app_redraw();
        }
        if (app_ok())
        {
            if (row == SET_BACK)
            {
                app_back();
                return;
            }
            m_edit = true;
            app_redraw();
        }
    }

    if (app_take_redraw())
    {
        static char storage[8];
        const char *values[12];
        const char *labels[12];

        uint8_t n = c_pages[m_page].count;
        if (n > 12)
            n = 12;

        for (uint8_t i = 0; i < n; i++)
        {
            labels[i] = c_pages[m_page].labels[i];
            if (c_pages[m_page].rows[i] == SET_BACK)
                values[i] = 0;
            else
                row_value(c_pages[m_page].rows[i], storage, &values[i]);
        }

        char title[16];
        // "SETTINGS: RADIO" style, or the edit marker
        strcpy(title, m_edit ? "EDIT: " : "SET: ");
        uint8_t k = (uint8_t)strlen(title);
        const char *pn = page_names[m_page];
        while (*pn && k + 1 < sizeof(title))
            title[k++] = *pn++;
        title[k] = 0;

        ui_list(title, labels, n, m_sel, values);
    }
}

const app_screen_t scr_settings = {
    .name = "Settings",
    .group = APP_GROUP_SYSTEM,
    .enter = settings_enter,
    .tick = settings_tick,
    .leave = settings_leave,
};
