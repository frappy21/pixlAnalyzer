/**
 * System screens:
 *  - Presets: named scan configurations applied in one go (Spectrum menu)
 *  - Sentry: low power watch mode that wakes up on activity (Tools menu)
 *  - Help: the button gestures (top level menu)
 */
#include "app_config.h"
#include "display.h"
#include "led.h"
#include "power.h"
#include "scanner.h"
#include "scr_system.h"
#include "screens.h"
#include "settings.h"
#include "systime.h"
#include "ui.h"
#include "ui_sys.h"

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

typedef struct
{
    const char *name;
    const char *tag; // right hand column of the list
    uint8_t band;    // band_preset_t
    uint8_t dwell;   // RSSI samples per channel visit
    uint8_t wf_decim;
    uint8_t wf_mode; // waterfall_mode_t
} preset_t;

static const preset_t presets[] = {
    // The 2.4GHz ISM band at the default sweep: WiFi channels 1-13
    {"WiFi", "ISM", BAND_ISM, SCAN_DWELL_SAMPLES_DEFAULT, 2, WF_DITHER},
    // Only the three advertising channels, long visits so short packets land
    {"BLE", "ADV", BAND_BLE_ADV, SCAN_DWELL_SAMPLES_MAX, 1, WF_DITHER},
    // 2360..2500 with short visits, for a quick look at everything
    {"Wide", "EXT", BAND_EXTENDED, 16, 3, WF_DITHER},
    // ISM with long visits and a slow waterfall: rare, short bursts
    {"Deep", "ISM", BAND_ISM, SCAN_DWELL_SAMPLES_MAX, 6, WF_THRESHOLD},
};

#define PRESET_COUNT (sizeof(presets) / sizeof(presets[0]))
#define PRESET_ROWS (PRESET_COUNT + 1) // Back row first

static uint8_t m_preset_sel;

static bool preset_active(const preset_t *p)
{
    return g_settings.band == p->band && g_settings.dwell == p->dwell &&
           g_settings.wf_decim == p->wf_decim && g_settings.wf_mode == p->wf_mode;
}

static void preset_apply(const preset_t *p)
{
    g_settings.band = p->band;
    g_settings.dwell = p->dwell;
    g_settings.wf_decim = p->wf_decim;
    g_settings.wf_mode = p->wf_mode;
    g_settings.auto_floor = 1;

    scanner_set_dwell(g_settings.dwell);
    scr_scanner_apply_band();
    settings_mark_dirty();
    settings_save();
}

static void presets_enter(void)
{
    m_preset_sel = 1;
    for (uint8_t i = 0; i < PRESET_COUNT; i++)
    {
        if (preset_active(&presets[i]))
            m_preset_sel = (uint8_t)(i + 1);
    }
}

static void presets_tick(uint32_t now)
{
    if (app_left())
    {
        m_preset_sel = (uint8_t)((m_preset_sel + PRESET_ROWS - 1) % PRESET_ROWS);
        app_redraw();
    }
    if (app_right())
    {
        m_preset_sel = (uint8_t)((m_preset_sel + 1) % PRESET_ROWS);
        app_redraw();
    }
    if (app_ok())
    {
        if (m_preset_sel == 0)
        {
            app_back();
            return;
        }
        // Straight to the spectrum, which shows the result
        preset_apply(&presets[m_preset_sel - 1]);
        app_home_select(&scr_scanner);
        return;
    }

    if (app_take_redraw())
    {
        static char active_tag[8];
        const char *items[PRESET_ROWS];
        const char *values[PRESET_ROWS];

        items[0] = "Back";
        values[0] = 0;
        for (uint8_t i = 0; i < PRESET_COUNT; i++)
        {
            items[i + 1] = presets[i].name;
            values[i + 1] = presets[i].tag;
            if (preset_active(&presets[i]))
            {
                // The one in use gets a star
                uint8_t n = 0;
                for (const char *s = presets[i].tag; *s && n < sizeof(active_tag) - 2;)
                    active_tag[n++] = *s++;
                active_tag[n++] = '*';
                active_tag[n] = 0;
                values[i + 1] = active_tag;
            }
        }
        ui_list("PRESETS", items, PRESET_ROWS, m_preset_sel, values);
    }
}

const app_screen_t scr_presets = {
    .name = "Presets",
    .group = APP_GROUP_SPECTRUM,
    .enter = presets_enter,
    .tick = presets_tick,
};

// ---------------------------------------------------------------------------
// Sentry
// ---------------------------------------------------------------------------

// A click while armed lights the screen for this long
#define SENTRY_PEEK_MS 5000
// Armed screen refresh, the counters are all there is to update
#define SENTRY_REFRESH_MS 1000
// LED blink rate while an alert stands
#define SENTRY_LED_RATE 3

static bool m_sentry_active;
static bool m_sentry_dark;
static uint32_t m_sentry_last_sweep;
static uint32_t m_sentry_peek_until;
static uint32_t m_sentry_last_draw;
static uint32_t m_sentry_alert_ms;
static sentry_view_t m_sentry;

bool scr_sentry_active(void)
{
    return m_sentry_active;
}

// Dark: the LCD backlight off, the OLED at its lowest contrast. The screen
// stays readable up close, and costs next to nothing.
static void sentry_dark(bool dark)
{
    if (dark == m_sentry_dark)
        return;
    m_sentry_dark = dark;
#ifdef OLED_TYPE_SH1106
    display_set_contrast(dark ? 0 : g_settings.contrast);
#else
    display_set_backlight(dark ? 0 : g_settings.backlight);
#endif
    m_sentry.dimmed = dark;
}

static void sentry_banner(void)
{
    ui_sentry_banner(m_sentry.last_mhz, m_sentry.last_db);
}

// The strongest channel of the last sweep above floor + threshold, 0 if none
static uint8_t sentry_check(uint16_t *mhz)
{
    uint8_t best = 0;
    for (uint8_t i = 0; i < scanner_count(); i++)
    {
        uint8_t peak = g_scan[i].peak; // -dBm, smaller is stronger
        if (peak == RSSI_INVALID || g_floor[i] <= peak)
            continue;
        uint8_t db = (uint8_t)(g_floor[i] - peak);
        if (db > best)
        {
            best = db;
            *mhz = scanner_mhz(i);
        }
    }
    return best >= g_settings.sentry_db ? best : 0;
}

static void sentry_alert(uint32_t now, uint16_t mhz, uint8_t db)
{
    m_sentry.alerts++;
    m_sentry.last_mhz = mhz;
    m_sentry.last_db = db;
    m_sentry_alert_ms = now;

    if (!m_sentry.alert)
    {
        m_sentry.alert = true;
        sentry_dark(false);
        display_set_overlay(sentry_banner);
        led_set_rate(SENTRY_LED_RATE);
    }
    app_redraw();
}

static void sentry_acknowledge(uint32_t now)
{
    m_sentry.alert = false;
    display_set_overlay(0);
    led_off();
    m_sentry_peek_until = now + SENTRY_PEEK_MS;
    app_redraw();
}

static void sentry_enter(void)
{
    m_sentry_active = true;
    m_sentry = (sentry_view_t){0};
    m_sentry_dark = false;

    uint32_t now = systime_ms();
    m_sentry_peek_until = now + SENTRY_PEEK_MS; // show the screen before going dark
    m_sentry_last_sweep = now;
    led_off();
}

static void sentry_leave(void)
{
    m_sentry_active = false;
    if (m_sentry.alert)
        display_set_overlay(0);
    led_off();
    sentry_dark(false);
}

static void sentry_tick(uint32_t now)
{
    uint32_t period = (uint32_t)g_settings.sentry_period * 100u;
    m_sentry.threshold = g_settings.sentry_db;
    m_sentry.period_ms = (uint16_t)period;

    // Reduced duty cycle: one sweep (a few ms) per period, radio and crystal
    // off in between while the main loop idles
    if (now - m_sentry_last_sweep >= period)
    {
        m_sentry_last_sweep = now;
        scanner_sweep();
        scanner_stop();
        power_hfxo_release();
        m_sentry.sweeps++;

        uint16_t mhz = 0;
        uint8_t db = sentry_check(&mhz);
        if (db)
            sentry_alert(now, mhz, db);
    }

    if (app_any())
    {
        if (m_sentry.alert)
            sentry_acknowledge(now);
        else
            m_sentry_peek_until = now + SENTRY_PEEK_MS;
        app_redraw();
    }

    if (!m_sentry.alert)
    {
        bool peek = (int32_t)(m_sentry_peek_until - now) > 0;
        if (peek == m_sentry_dark)
        {
            sentry_dark(!peek);
            app_redraw();
        }
    }

    led_update(now);

    if (now - m_sentry_last_draw >= SENTRY_REFRESH_MS)
        app_redraw();

    if (app_take_redraw())
    {
        m_sentry.last_age_s = m_sentry.last_mhz ? (now - m_sentry_alert_ms) / 1000u : 0;
        ui_sentry(&m_sentry);
        m_sentry_last_draw = now;
    }
}

const app_screen_t scr_sentry = {
    .name = "Sentry",
    .group = APP_GROUP_TOOLS,
    .enter = sentry_enter,
    .tick = sentry_tick,
    .leave = sentry_leave,
};

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

static uint8_t m_help_first;

static void help_enter(void)
{
    m_help_first = 0;
}

static void help_tick(uint32_t now)
{
    uint8_t last = (uint8_t)(ui_help_lines() - ui_help_rows());

    if (app_left() && m_help_first > 0)
    {
        m_help_first--;
        app_redraw();
    }
    if (app_right() && m_help_first < last)
    {
        m_help_first++;
        app_redraw();
    }
    if (app_ok())
    {
        app_back();
        return;
    }

    if (app_take_redraw())
        ui_help(m_help_first);
}

const app_screen_t scr_help = {
    .name = "Help",
    .group = APP_GROUP_SYSTEM,
    .enter = help_enter,
    .tick = help_tick,
};
