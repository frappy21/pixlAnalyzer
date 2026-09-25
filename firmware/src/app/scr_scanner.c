/**
 * Home screen (the scanner) plus the screens and menu actions that work on
 * the same sweep: Busiest, Meter, Overlay, Freeze, Set ref, Clear max, View,
 * Trace, RBW, Delta mkr, Cal offset, Alarm, Adapt dwell.
 *
 * The options set from here are session only (static variables, defaults
 * after every boot); nothing new goes into the settings record.
 */
#include <string.h>

#include "app_config.h"
#include "buttons.h"
#include "channels.h"
#include "display.h"
#include "gfx.h"
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
    .layout = LAYOUT_SPLIT,
};

static uint32_t m_sweep_count;
static uint32_t m_sweep_window_ms;

// Peak tool: rank of the peak the marker was last sent to, -1 for none yet
static int8_t m_peak_rank = -1;

// With the delta readout in the status bar, the tool name comes back for a
// moment after the tool changes
#define TOOL_HINT_MS 1500
static uint32_t m_tool_hint_until;

// Interference alarm: threshold in dB over the floor, 0 is off. Once raised
// it stays up ALARM_HOLD_MS after the last sweep that exceeded it, so a
// single burst is still readable.
#define ALARM_HOLD_MS 1500
#define ALARM_LED_RATE 8 // blinks per second while up
static const uint8_t alarm_levels[] = {0, 10, 15, 20, 30};
static uint8_t m_alarm_sel;
static uint32_t m_alarm_until;

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

    case TOOL_PEAK:
    {
        // RIGHT: the next lower peak, back to the strongest after the last.
        // LEFT: the next higher one, stopping at the strongest.
        int rank = direction > 0 ? m_peak_rank + 1 : (m_peak_rank > 0 ? m_peak_rank - 1 : 0);
        uint8_t idx;
        if (!spectrum_find_peak((uint8_t)rank, &idx))
        {
            rank = 0;
            if (!spectrum_find_peak(0, &idx))
                break; // nothing stands out, the marker stays
        }
        m_peak_rank = (int8_t)rank;
        m_view.marker_col = spectrum_chan_to_col(idx);
        break;
    }

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
        uint8_t shown = ui_scanner_waterfall_rows(m_view.layout);
        if (shown == 0)
            shown = WATERFALL_ROWS; // no waterfall on screen, keep the usual bound
        if (rows > shown && back > rows - shown)
            back = rows - shown;
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

static void scanner_leave(void)
{
    if (m_view.alarm)
        led_off();
    m_view.alarm = false;
    m_alarm_until = 0;
}

// Raises the interference alarm on a fresh sweep and drops it once the hold
// time has run out, frozen or not
static void alarm_check(uint32_t now, bool fresh)
{
    uint8_t thr = alarm_levels[m_alarm_sel];
    if (thr && fresh)
    {
        uint8_t idx = spectrum_strongest();
        uint8_t db = spectrum_db(idx);
        if (db >= thr)
        {
            // Keep the strongest offender of the episode on the banner
            if (!m_view.alarm || db >= m_view.alarm_db || idx == m_view.alarm_chan)
            {
                m_view.alarm_chan = idx;
                m_view.alarm_db = db;
            }
            m_alarm_until = now + ALARM_HOLD_MS;
        }
    }

    bool up = thr && (int32_t)(m_alarm_until - now) > 0;
    if (up != m_view.alarm)
    {
        m_view.alarm = up;
        if (!up)
            led_off();
        app_redraw();
    }
    if (up)
    {
        // Set on every pass: the Meter turns the LED off when it closes
        led_set_rate(ALARM_LED_RATE);
        led_update(now);
    }
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
    alarm_check(now, !m_view.frozen);

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
        if (m_view.tool == TOOL_PEAK)
            m_peak_rank = -1; // the first RIGHT goes to the strongest
        m_tool_hint_until = now + TOOL_HINT_MS;
        app_redraw();
    }

    if (app_take_redraw())
    {
        m_view.tool_hint = (int32_t)(m_tool_hint_until - now) > 0;
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
    .leave = scanner_leave,
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
    spectrum_clear_max(); // min hold too
    app_open(&scr_scanner); // straight to Spectrum to watch it build up again
}

const app_screen_t act_clear_max = {
    .name = "Clear max",
    .group = APP_GROUP_SPECTRUM,
    .action = clear_max_action,
};

static void view_action(void)
{
    m_view.layout = (uint8_t)((m_view.layout + 1) % LAYOUT_COUNT);

    // A taller waterfall reaches less far back
    uint16_t rows = spectrum_history_rows();
    uint8_t shown = ui_scanner_waterfall_rows(m_view.layout);
    if (shown && rows > shown && m_view.scroll_back > rows - shown)
        m_view.scroll_back = rows - shown;
}

static const char *view_value(void)
{
    return ui_layout_name(m_view.layout);
}

const app_screen_t act_view = {
    .name = "View",
    .group = APP_GROUP_SPECTRUM,
    .action = view_action,
    .value = view_value,
};

static void trace_action(void)
{
    spectrum_set_trace((uint8_t)((spectrum_trace() + 1) % TRACE_COUNT));
}

static const char *trace_value(void)
{
    return spectrum_trace_name(spectrum_trace());
}

const app_screen_t act_trace = {
    .name = "Trace",
    .group = APP_GROUP_SPECTRUM,
    .action = trace_action,
    .value = trace_value,
};

static void rbw_action(void)
{
    spectrum_set_rbw(spectrum_rbw() == 1 ? 2 : 1);
}

static const char *rbw_value(void)
{
    return spectrum_rbw() == 2 ? "2MHZ" : "1MHZ";
}

const app_screen_t act_rbw = {
    .name = "RBW",
    .group = APP_GROUP_SPECTRUM,
    .action = rbw_action,
    .value = rbw_value,
};

static void delta_action(void)
{
    // Anchors the reference at the marker; the marker then moves on its own
    m_view.delta_mhz = m_view.delta_mhz ? 0 : scr_scanner_marker_mhz();
}

static const char *delta_value(void)
{
    static char buf[8];
    if (!m_view.delta_mhz)
        return "OFF";
    gfx_fmt_int(buf, m_view.delta_mhz);
    return buf;
}

const app_screen_t act_delta = {
    .name = "Delta mkr",
    .group = APP_GROUP_SPECTRUM,
    .action = delta_action,
    .value = delta_value,
};

// Steps of 2 dB: the radio's own RSSI accuracy is +-2 dB, a finer step would
// pretend to more than it can deliver
#define CAL_STEP_DB 2

static void cal_action(void)
{
    int next = spectrum_cal() + CAL_STEP_DB;
    if (next > SPECTRUM_CAL_MAX_DB)
        next = SPECTRUM_CAL_MIN_DB;
    spectrum_set_cal((int8_t)next);
}

static const char *cal_value(void)
{
    static char buf[8];
    char *p = buf;
    if (spectrum_cal() > 0)
        *p++ = '+';
    gfx_fmt_int(p, spectrum_cal());
    while (*p)
        p++;
    memcpy(p, "DB", 3);
    return buf;
}

const app_screen_t act_cal = {
    .name = "Cal offset",
    .group = APP_GROUP_SPECTRUM,
    .action = cal_action,
    .value = cal_value,
};

static void alarm_action(void)
{
    m_alarm_sel = (uint8_t)((m_alarm_sel + 1) % sizeof(alarm_levels));
    if (!alarm_levels[m_alarm_sel] && m_view.alarm)
    {
        m_view.alarm = false;
        led_off();
    }
    m_alarm_until = 0;
}

static const char *alarm_value(void)
{
    static char buf[8];
    uint8_t thr = alarm_levels[m_alarm_sel];
    if (!thr)
        return "OFF";
    gfx_fmt_int(buf, thr);
    memcpy(buf + strlen(buf), "DB", 3);
    return buf;
}

const app_screen_t act_alarm = {
    .name = "Alarm",
    .group = APP_GROUP_SPECTRUM,
    .action = alarm_action,
    .value = alarm_value,
};

static void dwell_action(void)
{
    scanner_set_adaptive(!scanner_adaptive());
}

static const char *dwell_value(void)
{
    return scanner_adaptive() ? "ON" : "OFF";
}

const app_screen_t act_dwell = {
    .name = "Adapt dwell",
    .group = APP_GROUP_SPECTRUM,
    .action = dwell_action,
    .value = dwell_value,
};
