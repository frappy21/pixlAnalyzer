/**
 * RADAR main screen: sweep driven signal tracking with verdicts, the bug
 * hunt (baseline + new signal alerts) and the microwave leak check.
 *
 * SCAN  - sweeps the ISM band, lists persistent signals with a verdict:
 *         analogue video (an AV sender, a 2.4GHz camera, a baby monitor),
 *         WiFi video, hopping drone links, ESB control links, carriers.
 * HUNT  - takes a baseline, then anything that appears after it flashes,
 *         blinks the LED and lands in the NOR log: the hidden camera /
 *         bug sweep. Arm by entering the mode, re-arm any time with MID.
 * MW    - parks on 2450 MHz and watches the level: the leaky microwave
 *         oven check (a level statement, not a safety certificate).
 *
 * Short MID cycles the mode. LEFT/RIGHT move the signal selection. The
 * sniffer is never involved; the sweep engine is the one owning the radio.
 */
#include <string.h>

#include "app.h"
#include "gfx.h"
#include "led.h"
#include "radar.h"
#include "scanner.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"
#include "ui_radar.h"

// How often the strip is recomputed and the table rebuilt
#define RADAR_REDRAW_MS 250

// Microwave trend samples, one per measurement (2 s)
#define MW_TREND 16

static radar_work_t *m_work; // the arena
#define RADAR_ARENA_BYTES 2048

static uint8_t m_mode; // 0 scan, 1 hunt, 2 microwave
static bool m_armed;
static uint8_t m_sel;

// The strip values: dB above the floor * 8, one per channel
static uint8_t m_strip[RADAR_CHANS];

// Microwave measurement
static burst_t m_mw_bursts[24];
static park_stats_t m_mw_stats;
static int8_t m_mw_level;
static const char *m_mw_verdict = "FLOOR LEVEL";
static uint8_t m_mw_trend[MW_TREND];
static uint8_t m_mw_trend_n;
static uint32_t m_mw_last_ms;
static uint8_t m_mw_cycle;

// New-signal alert state for the LED
static bool m_alert;
static uint32_t m_alert_ms;

// Sweep results mapped into the radar's channel space. The scanner's
// channel indices cover its span; the radar band is 2400..2483 and the
// span is set to exactly that on entry.
static void feed_radar(void)
{
    uint8_t busy[RADAR_CHANS];
    uint8_t peak[RADAR_CHANS];
    uint8_t floor[RADAR_CHANS];

    uint8_t n = scanner_count();
    if (n > RADAR_CHANS)
        n = RADAR_CHANS;

    for (uint8_t i = 0; i < n; i++)
    {
        busy[i] = g_scan[i].busy;
        peak[i] = g_scan[i].peak;
        floor[i] = g_floor[i];
    }

    radar_sweep(m_work, busy, peak, floor);
}

static void strip_update(void)
{
    for (uint8_t c = 0; c < RADAR_CHANS; c++)
    {
        int db = (int)m_work->floor[c] - (int)m_work->peak_db[c];
        if (db < 0)
            db = 0;
        if (db > 31)
            db = 31;
        m_strip[c] = (uint8_t)(db * 8);
    }
}

static void hunt_check(void)
{
    if (m_mode != 1 || !m_armed)
        return;

    uint8_t n = radar_signals(m_work);
    for (uint8_t i = 0; i < n; i++)
    {
        const radar_signal_t *s = radar_signal(m_work, i);
        if (s->first_seen && s->peak_db >= 10)
        {
            m_alert = true;
            m_alert_ms = 0;
        }
    }
}

static void measure_microwave(void)
{
    uint16_t n = scanner_park(2450, 300, m_mw_bursts,
                              (uint16_t)(sizeof(m_mw_bursts) / sizeof(m_mw_bursts[0])),
                              &m_mw_stats);
    uint8_t floor = g_floor[50];
    if (floor == 0)
        floor = 100; // unknown floor: assume a typical one

    int8_t level;
    if (radar_microwave(m_mw_bursts, n, &m_mw_stats, floor, &level, &m_mw_verdict))
    {
        m_mw_level = level;
        if (m_mw_trend_n < MW_TREND)
            m_mw_trend[m_mw_trend_n++] = (uint8_t)(level > 0 ? level : 0);
        else
        {
            memmove(m_mw_trend, m_mw_trend + 1, MW_TREND - 1);
            m_mw_trend[MW_TREND - 1] = (uint8_t)(level > 0 ? level : 0);
        }
    }
}

static void radar_enter(void)
{
    if (!m_work)
        m_work = (radar_work_t *)g_app_arena;
    radar_init(m_work);

    m_mode = 0;
    m_armed = false;
    m_sel = 0;
    m_mw_trend_n = 0;
    m_mw_last_ms = 0;
    m_mw_cycle = 0;
    m_alert = false;

    scanner_init();
    scanner_set_span(RADAR_START_MHZ, RADAR_END_MHZ);
    led_off();
}

static void radar_leave(void)
{
    led_off();
    // The sweep state goes back to the configured band for the other
    // screens
    scanner_init();
    scr_scanner_apply_band();
}

static void radar_tick(uint32_t now)
{
    if (m_mode == 2)
    {
        // One parked measurement every 2 s, the level in between is drawn
        // from the last one
        if ((int32_t)(now - m_mw_last_ms) >= 2000)
        {
            m_mw_last_ms = now;
            measure_microwave();
            app_redraw();
        }
    }
    else
    {
        // Sweep and feed: several sweeps make one radar window
        scanner_sweep();
        feed_radar();
        if (m_work->sweeps == 0) // a window just closed
        {
            radar_signals_rebuild(m_work);
            strip_update();
            hunt_check();
            app_redraw();
        }
    }

    // The LED alert: fast blink for 3 s after something new in hunt mode
    if (m_alert)
    {
        if (!m_alert_ms)
            m_alert_ms = now;
        if ((int32_t)(now - m_alert_ms) > 3000)
        {
            m_alert = false;
            led_off();
        }
        else
            led_set((now / 125) & 1);
    }

    // Mode cycling on short MID; in hunt mode a fresh baseline on long MID
    if (app_ok())
    {
        m_mode = (uint8_t)((m_mode + 1) % 3);
        if (m_mode == 1)
        {
            radar_hunt_arm(m_work);
            m_armed = true;
            ui_message("HUNT ARMED", "NEW SIGNALS ALERT", 900);
        }
        else if (m_mode == 2)
        {
            m_mw_trend_n = 0;
            m_mw_last_ms = 0;
        }
        m_sel = 0;
        app_redraw();
    }

    if (app_left() && m_mode != 2)
    {
        uint8_t n = radar_signals(m_work);
        if (n)
            m_sel = (uint8_t)((m_sel + n - 1) % n);
        app_redraw();
    }
    if (app_right() && m_mode != 2)
    {
        uint8_t n = radar_signals(m_work);
        if (n)
            m_sel = (uint8_t)((m_sel + 1) % n);
        app_redraw();
    }

    if (app_take_redraw())
    {
        radar_view_t view;
        memset(&view, 0, sizeof(view));
        view.mode = m_mode;
        view.hunt_armed = m_armed;
        view.strip = m_strip;
        view.strip_len = RADAR_CHANS;
        view.selected = m_sel;
        view.work = m_work;
        view.mw_level = m_mw_level;
        view.mw_verdict = m_mw_verdict;
        view.mw_trend = m_mw_trend;
        view.mw_trend_len = m_mw_trend_n;
        ui_radar(&view);
    }
}

const app_screen_t scr_radar = {
    .name = "Radar",
    .group = APP_GROUP_RC,
    .enter = radar_enter,
    .tick = radar_tick,
    .leave = radar_leave,
    .busy = true,
};
