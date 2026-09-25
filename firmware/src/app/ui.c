#include <string.h>

#include "nrf_delay.h"

#include "app_config.h"
#include "battery.h"
#include "board_config.h"
#include "buttons.h"
#include "channels.h"
#include "display.h"
#include "font3x5.h"
#include "gfx.h"
#include "power.h"
#include "scanner.h"
#include "settings.h"
#include "spectrum.h"
#include "systime.h"
#include "ui.h"

#define LIST_ROWS 6
#define LIST_TOP 10

static const char *tool_name(uint8_t tool)
{
    switch (tool)
    {
    case TOOL_MARK:
        return "MARK";
    case TOOL_PEAK:
        return "PEAK";
    case TOOL_SPAN:
        return "SPAN";
    case TOOL_WFALL:
        return "WFALL";
    case TOOL_SCROLL:
        return "HIST";
    default:
        return "";
    }
}

// ---------------------------------------------------------------------------
// Common furniture
// ---------------------------------------------------------------------------

void ui_battery(void)
{
    // The voltage itself instead of a gauge: the percentage is a curve fitted
    // over a reading that sags under load, the volts are what was measured
    char buf[12];
    char *p = buf;

    if (g_battery.charging)
        *p++ = '+';

    if (g_battery.valid)
    {
        gfx_fmt_fixed(p, (g_battery.mv + 5) / 10, 2);
        while (*p)
            p++;
    }
    else
    {
        memcpy(p, "?.??", 4);
        p += 4;
    }
    *p++ = 'V';
    *p = '\0';

    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 1, buf);
}

void ui_title(const char *title)
{
    gfx_text(2, 1, title);
    gfx_hline(0, DISP_W - 1, 9);
}

// ---------------------------------------------------------------------------
// Boot and gates
// ---------------------------------------------------------------------------

void ui_boot_screen(void)
{
    char buf[16];

    display_clear();

    gfx_box(2, 2, 124, 60, false, true);
    gfx_text(46, 8, "2.4GHz");
    gfx_text(40, 17, "SPECTRUM");
    gfx_text(40, 26, "ANALYZER");

    // Why did we boot? WATCHDOG or a bare POWER ON after the device seemed to
    // switch itself off is the difference between a software and a supply fault.
    gfx_text_micro(6, 38, "RESET");
    gfx_text_micro(34, 38, power_reset_reason_name());

    gfx_text_micro(6, 45, "BATT");
    gfx_fmt_fixed(buf, g_battery.mv_raw, 3);
    gfx_text_micro(34, 45, buf);
    gfx_text_micro(34 + gfx_text_micro_width(buf) + 2, 45, "V");
    gfx_fmt_int(buf, g_battery.raw_adc);
    gfx_text_micro(80, 45, buf);
    gfx_text_micro(80 + gfx_text_micro_width(buf) + 2, 45, "ADC");

    gfx_text_micro(6, 52, "SLEEP ONLY FROM THE MENU");

    display_flush();
    nrf_delay_ms(2500);
}

void ui_power_on_gate(void)
{
    // The hold-to-start gate is gone. It was the only path where releasing a
    // button could switch the device off, and a device that switches itself
    // off is impossible to debug. Sleep is now a menu item and nothing else.
    buttons_flush();
}

void ui_message(const char *line1, const char *line2, uint32_t hold_ms)
{
    display_clear();
    gfx_text(64 - gfx_text_width(line1) / 2, line2 ? 24 : 30, line1);
    if (line2)
        gfx_text(64 - gfx_text_width(line2) / 2, 36, line2);
    display_flush();

    if (hold_ms)
        nrf_delay_ms(hold_ms);
}

void ui_low_battery(void)
{
    display_clear();
    gfx_box(10, 14, 108, 36, false, true);
    gfx_text(30, 20, "BATTERY LOW");

    char buf[12];
    char *p = gfx_fmt_fixed(buf, g_battery.mv, 3);
    (void)p;
    gfx_text(40, 34, buf);
    gfx_text_micro(20, 42, "CHARGE IT, STILL RUNNING");
    display_flush();
}

// ---------------------------------------------------------------------------
// Scanner
// ---------------------------------------------------------------------------

// Layout of the scanner screen below the status bar: the plot (h 0 for none),
// the ruler line and the waterfall (rows 0 for none)
typedef struct
{
    uint8_t plot_top;
    uint8_t plot_h;
    uint8_t ruler_y;
    uint8_t wf_top;
    uint8_t wf_rows;
} scanner_geom_t;

static const scanner_geom_t geom[LAYOUT_COUNT] = {
    [LAYOUT_SPLIT] = {SPECTRUM_TOP, SPECTRUM_H, RULER_Y, WATERFALL_START, WATERFALL_ROWS},
    [LAYOUT_SPECTRUM] = {STATUS_H, DISP_H - STATUS_H - RULER_H, DISP_H - RULER_H, 0, 0},
    [LAYOUT_WATERFALL] = {0, 0, STATUS_H, STATUS_H + RULER_H, DISP_H - STATUS_H - RULER_H},
    [LAYOUT_OCC] = {0, 0, 0, 0, 0},
};

uint8_t ui_scanner_waterfall_rows(uint8_t layout)
{
    return layout < LAYOUT_COUNT ? geom[layout].wf_rows : 0;
}

const char *ui_layout_name(uint8_t layout)
{
    switch (layout)
    {
    case LAYOUT_SPECTRUM:
        return "SPECT";
    case LAYOUT_WATERFALL:
        return "WFALL";
    case LAYOUT_OCC:
        return "OCCUP";
    default:
        return "SPLIT";
    }
}

// Signed figure with an explicit plus, then a unit
static int text_signed(int x, int y, int value, const char *unit)
{
    char buf[12];
    char *p = buf;
    if (value >= 0)
        *p++ = '+';
    gfx_fmt_int(p, value);
    gfx_text_micro(x, y, buf);
    x += gfx_text_micro_width(buf) + 1;
    gfx_text_micro(x, y, unit);
    return x + gfx_text_micro_width(unit);
}

// Channel index of an absolute frequency, -1 when it is not swept
static int chan_of_mhz(uint16_t mhz)
{
    for (uint8_t i = 0; i < scanner_count(); i++)
    {
        if (scanner_mhz(i) == mhz)
            return i;
    }
    return -1;
}

static void scanner_status_bar(const scanner_view_t *view)
{
    char buf[16];

    uint8_t idx = spectrum_col_to_chan(view->marker_col);
    uint16_t mhz = scanner_mhz(idx);
    int level = 0;
    bool has_level = spectrum_level_dbm(idx, &level);

    if (view->delta_mhz && !view->tool_hint && !view->frozen)
    {
        // Left: marker minus reference, in MHz and dB. The calibration
        // offset cancels out of the difference.
        int x = text_signed(0, 1, (int)mhz - (int)view->delta_mhz, "M");
        int ref = chan_of_mhz(view->delta_mhz);
        int ref_level = 0;
        if (has_level && ref >= 0 && spectrum_level_dbm((uint8_t)ref, &ref_level))
            text_signed(x + 3, 1, level - ref_level, "DB");
        else
            gfx_text_micro(x + 3, 1, "--DB");
    }
    else
    {
        // Left: sweeps per second, the honest measure of how much air we see
        gfx_fmt_int(buf, (int)view->sweeps_s);
        int x = 0;
        gfx_text_micro(x, 1, buf);
        x += gfx_text_micro_width(buf) + 3;
        gfx_text_micro(x, 1, "HZ");
        x += gfx_text_micro_width("HZ") + 5;

        // Middle: current tool, or the frozen marker
        gfx_text_micro(x, 1, view->frozen ? "FROZEN" : tool_name(view->tool));
    }

    // Marker readout: frequency and calibrated level
    gfx_fmt_int(buf, mhz);
    gfx_text_micro(46, 1, buf);

    if (has_level)
    {
        gfx_fmt_int(buf, level);
        gfx_text_micro(72, 1, buf);
    }

    // Trace and RBW when they are not the defaults: AV, MN, 2M
    char *p = buf;
    if (spectrum_trace() == TRACE_AVG)
    {
        *p++ = 'A';
        *p++ = 'V';
    }
    else if (spectrum_trace() == TRACE_MIN)
    {
        *p++ = 'M';
        *p++ = 'N';
    }
    if (spectrum_rbw() == 2)
    {
        *p++ = '2';
        *p++ = 'M';
    }
    *p = '\0';
    gfx_text_micro(88, 1, buf);

    // Right edge, x=104 onwards for "+4.12V": clear of the tags at x=88
    ui_battery();
}

// Marker and delta marker over the waterfall, when there is no plot for them
static void waterfall_markers(const scanner_geom_t *g, int marker_col, int delta_col)
{
    for (int y = g->wf_top; y < g->wf_top + g->wf_rows; y++)
    {
        int phase = (y - g->wf_top) & 3;
        if (phase == 0 && marker_col >= 0)
            gfx_pixel(marker_col, y, !gfx_pixel_get(marker_col, y));
        if (phase == 2 && delta_col >= 0)
            gfx_pixel(delta_col, y, !gfx_pixel_get(delta_col, y));
    }
}

// Inverted banner: which channel is how far above its floor
static void alarm_banner(const scanner_view_t *view, int y)
{
    char buf[24];
    char *p = buf;
    memcpy(p, "ALARM ", 6);
    p += 6;
    gfx_fmt_int(p, scanner_mhz(view->alarm_chan));
    while (*p)
        p++;
    *p++ = ' ';
    *p++ = '+';
    gfx_fmt_int(p, view->alarm_db);
    while (*p)
        p++;
    memcpy(p, "DB", 3);

    int w = gfx_text_micro_width(buf);
    int x = DISP_W - 2 - w;
    gfx_box(x - 1, y - 1, w + 2, MICRO_HEIGHT + 2, true, false);
    gfx_text_micro(x, y, buf);
    gfx_invert(x - 1, y - 1, w + 2, MICRO_HEIGHT + 2);
}

// Channel occupancy bar chart: WiFi channels 1-13 + BLE advertising channels
#define OCC_WIFI_N   13
#define OCC_BAR_W    8
#define OCC_BAR_GAP  1
#define OCC_STRIDE   (OCC_BAR_W + OCC_BAR_GAP)
#define OCC_LEFT     5
#define OCC_BOT      50
#define OCC_MAX_H    38
#define OCC_LABEL_Y  52

static void draw_occ_layout(const scanner_view_t *view)
{
    char buf[8];

    display_clear();
    scanner_status_bar(view);

    // 30% threshold line so busy channels are obvious
    gfx_hline(OCC_LEFT, OCC_LEFT + OCC_WIFI_N * OCC_STRIDE - 1,
              OCC_BOT - (30 * OCC_MAX_H) / 100);

    for (uint8_t i = 0; i < OCC_WIFI_N; i++)
    {
        chan_mark_t mark;
        if (!channels_plan_get(PLAN_WIFI, i, &mark))
            break;

        int x = OCC_LEFT + i * OCC_STRIDE;
        uint8_t occ = channels_occupancy(PLAN_WIFI, i);
        int h = (int)((uint32_t)occ * OCC_MAX_H / 255);
        if (h < 1 && occ > 0)
            h = 1;

        gfx_box(x, OCC_BOT - OCC_MAX_H, OCC_BAR_W, OCC_MAX_H, false, true);
        if (h > 0)
            gfx_box(x, OCC_BOT - h, OCC_BAR_W, h, true, true);

        // Channel number: 1 digit centered, 2 digits left-aligned
        if (mark.number <= 9)
        {
            gfx_fmt_int(buf, mark.number);
            gfx_text_micro(x + 2, OCC_LABEL_Y, buf);
        }
        else
        {
            buf[0] = (char)('0' + mark.number / 10);
            buf[1] = (char)('0' + mark.number % 10);
            buf[2] = '\0';
            gfx_text_micro(x, OCC_LABEL_Y, buf);
        }
    }

    // BLE advertising channels in the footer row
    gfx_hline(0, DISP_W - 1, OCC_LABEL_Y + 6);
    gfx_text_micro(2, OCC_LABEL_Y + 8, "BLE");

    uint8_t ble_n = channels_plan_count(PLAN_BLE);
    int bx = 24;
    for (uint8_t i = 0; i < ble_n && i < 3; i++)
    {
        chan_mark_t mark;
        if (!channels_plan_get(PLAN_BLE, i, &mark))
            break;
        uint8_t occ = channels_occupancy(PLAN_BLE, i);
        uint8_t pct = (uint8_t)(((uint32_t)occ * 100) / 255);
        gfx_fmt_int(buf, mark.number);
        gfx_text_micro(bx, OCC_LABEL_Y + 8, buf);
        bx += gfx_text_micro_width(buf) + 1;
        gfx_text_micro(bx, OCC_LABEL_Y + 8, ":");
        bx += gfx_text_micro_width(":") + 1;
        gfx_fmt_int(buf, pct);
        gfx_text_micro(bx, OCC_LABEL_Y + 8, buf);
        bx += gfx_text_micro_width(buf) + 1;
        gfx_text_micro(bx, OCC_LABEL_Y + 8, "%");
        bx += gfx_text_micro_width("%") + 6;
    }

    display_flush();
}

void ui_scanner(const scanner_view_t *view)
{
    if (view->layout == LAYOUT_OCC)
    {
        draw_occ_layout(view);
        return;
    }

    const scanner_geom_t *g = &geom[view->layout < LAYOUT_COUNT ? view->layout : LAYOUT_SPLIT];

    display_clear();

    scanner_status_bar(view);

    int delta_col = -1;
    if (view->delta_mhz)
    {
        int ref = chan_of_mhz(view->delta_mhz);
        if (ref >= 0)
            delta_col = spectrum_chan_to_col((uint8_t)ref);
    }

    if (g->plot_h)
    {
        spectrum_draw_plot(g->plot_top, g->plot_h, view->marker_col, delta_col);
        spectrum_draw_db_labels(g->plot_top, g->plot_h);
    }
    spectrum_draw_ruler_at(g->ruler_y, view->plan, view->marker_col);

    if (g->wf_rows)
    {
        spectrum_draw_waterfall_rows(g->wf_top, g->wf_rows, view->scroll_back);
        if (!g->plot_h)
            waterfall_markers(g, view->marker_col, delta_col);

        // Scroll position indicator on the right edge of the waterfall
        if (view->scroll_back)
        {
            uint16_t rows = spectrum_history_rows();
            if (rows > g->wf_rows)
            {
                int y = g->wf_top + (int)((uint32_t)view->scroll_back * (g->wf_rows - 1) / rows);
                gfx_vline(DISP_W - 1, y, y + 1);
            }
        }
    }

    if (view->alarm)
        alarm_banner(view, (g->plot_h ? g->plot_top : g->wf_top) + 1);

    display_flush();
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

void ui_list(const char *title, const char *const *items, uint8_t count, uint8_t selected,
             const char *const *values)
{
    display_clear();
    ui_title(title);

    uint8_t first = 0;
    if (selected >= LIST_ROWS)
        first = selected - (LIST_ROWS - 1);
    if (count > LIST_ROWS && first > count - LIST_ROWS)
        first = count - LIST_ROWS;

    for (uint8_t row = 0; row < LIST_ROWS && first + row < count; row++)
    {
        uint8_t i = first + row;
        int y = LIST_TOP + row * 9;

        gfx_text(4, y, items[i]);
        if (values && values[i])
        {
            int w = gfx_text_width(values[i]);
            gfx_text(DISP_W - 3 - w, y, values[i]);
        }
        if (i == selected)
            gfx_invert(0, y - 1, DISP_W, 9);
    }

    // Scrollbar when the list does not fit
    if (count > LIST_ROWS)
    {
        int track = DISP_H - LIST_TOP;
        int h = (track * LIST_ROWS) / count;
        int y = LIST_TOP + (track * first) / count;
        gfx_box(DISP_W - 1, y, 1, h, true, true);
    }

    display_flush();
}

// ---------------------------------------------------------------------------
// Top channels and the WiFi advisor
// ---------------------------------------------------------------------------

void ui_top_channels(void)
{
    char buf[16];
    uint8_t idx[5];
    uint8_t n = spectrum_top_busy(idx, 5);

    display_clear();
    ui_title("BUSIEST");
    gfx_text_micro(60, 2, "LAST 30S");

    for (uint8_t i = 0; i < n; i++)
    {
        int y = 11 + i * 8;
        uint16_t mhz = scanner_mhz(idx[i]);

        gfx_fmt_int(buf, mhz);
        gfx_text_micro(2, y, buf);

        const char *label = channels_label(mhz);
        if (label[0])
            gfx_text_micro(26, y, label);

        // Long window occupancy, 0..100 mapped to 60 pixels
        uint8_t occ = spectrum_occupancy(idx[i]);
        int w = (occ * 60) / 100;
        gfx_box(44, y, 62, 5, false, true);
        if (w > 0)
            gfx_box(45, y + 1, w, 3, true, true);

        gfx_fmt_int(buf, occ);
        gfx_text_micro(110, y, buf);
    }

    if (n == 0)
        gfx_text_micro(2, 20, "BAND IS QUIET");

    uint8_t score = 0;
    uint8_t best = channels_best_wifi(&score);
    gfx_hline(0, DISP_W - 1, 52);
    gfx_text_micro(2, 56, "BEST WIFI CH");
    gfx_fmt_int(buf, best);
    gfx_text_micro(56, 56, buf);
    gfx_text_micro(70, 56, "FREE");
    gfx_fmt_int(buf, (score * 100) / 255);
    gfx_text_micro(92, 56, buf);
    gfx_text_micro(104, 56, "PCT");

    display_flush();
}

// ---------------------------------------------------------------------------
// Meter / direction finding
// ---------------------------------------------------------------------------

void ui_meter(uint16_t mhz, uint8_t rssi, uint8_t db, const uint8_t *trend, uint8_t trend_len)
{
    char buf[16];
    display_clear();

    gfx_fmt_int(buf, mhz);
    gfx_text(2, 1, buf);
    gfx_text(2 + gfx_text_width(buf) + 3, 1, "MHz");
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    // Big level readout, drawn as a wide bar plus the number
    if (rssi == RSSI_INVALID)
    {
        gfx_text(40, 20, "NO RX");
    }
    else
    {
        gfx_fmt_int(buf, spectrum_dbm(rssi)); // calibrated
        gfx_text(4, 14, buf);
        gfx_text(4 + gfx_text_width(buf) + 4, 14, "dBm");
    }

    int w = (db * (DISP_W - 8)) / SPECTRUM_RANGE_DB;
    if (w > DISP_W - 8)
        w = DISP_W - 8;
    gfx_box(4, 26, DISP_W - 8, 8, false, true);
    if (w > 0)
        gfx_box(5, 27, w, 6, true, true);

    // Trend: the last samples, so you can see if you are getting warmer
    for (uint8_t i = 0; i < trend_len && i < DISP_W - 4; i++)
    {
        int h = (trend[i] * 20) / SPECTRUM_RANGE_DB;
        if (h > 20)
            h = 20;
        if (h > 0)
            gfx_vline(2 + i, 62 - h, 62);
    }
    gfx_hline(0, DISP_W - 1, 63);

    display_flush();
}

// ---------------------------------------------------------------------------
// Identify: the evidence card
// ---------------------------------------------------------------------------

void ui_identify_progress(uint16_t mhz, uint8_t percent)
{
    char buf[16];
    display_clear();
    ui_title("IDENTIFY");

    gfx_fmt_int(buf, mhz);
    gfx_text(30, 20, buf);
    gfx_text(30 + gfx_text_width(buf) + 4, 20, "MHz");

    gfx_text_micro(30, 32, "LISTENING");
    gfx_box(14, 40, 100, 8, false, true);
    gfx_box(16, 42, (percent * 96) / 100, 4, true, true);

    display_flush();
}

void ui_identify(uint16_t mhz, const verdict_t *v, bool ble_confirmed, uint16_t ble_packets)
{
    char buf[20];
    display_clear();

    // Header: frequency and what we think it is
    gfx_fmt_int(buf, mhz);
    gfx_text(2, 1, buf);
    const char *label = channels_label(mhz);
    if (label[0])
        gfx_text_micro(30, 2, label);
    gfx_hline(0, DISP_W - 1, 9);

    const char *name = ble_confirmed ? "BLE (DECODED)" : classify_name(v->kind);
    gfx_text(2, 12, name);

    // Confidence, stated as a number so nobody has to guess what we mean
    gfx_text_micro(2, 22, "CONFIDENCE");
    gfx_fmt_int(buf, ble_confirmed ? 99 : v->confidence);
    gfx_text_micro(50, 22, buf);
    gfx_text_micro(50 + gfx_text_micro_width(buf) + 2, 22, "PCT");

    // The evidence, which is the part that is actually measured
    gfx_text_micro(2, 31, "WIDTH");
    gfx_fmt_int(buf, v->f.width_mhz);
    gfx_text_micro(34, 31, buf);
    gfx_text_micro(34 + gfx_text_micro_width(buf) + 2, 31, "MHZ");

    gfx_text_micro(2, 38, "DUTY");
    gfx_fmt_fixed(buf, (int)(v->f.duty_ppm / 100), 2);
    gfx_text_micro(34, 38, buf);
    gfx_text_micro(34 + gfx_text_micro_width(buf) + 2, 38, "PCT");

    gfx_text_micro(2, 45, "BURST");
    gfx_fmt_fixed(buf, v->f.median_len_us / 10, 2);
    gfx_text_micro(34, 45, buf);
    gfx_text_micro(34 + gfx_text_micro_width(buf) + 2, 45, "MS");

    gfx_text_micro(70, 31, "PERIOD");
    if (v->f.period_us)
    {
        gfx_fmt_fixed(buf, (int)(v->f.period_us / 100), 1);
        gfx_text_micro(102, 31, buf);
        gfx_text_micro(102 + gfx_text_micro_width(buf) + 1, 31, "MS");
    }
    else
    {
        gfx_text_micro(102, 31, "NONE");
    }

    gfx_text_micro(70, 38, "BURSTS");
    gfx_fmt_int(buf, v->f.bursts);
    gfx_text_micro(102, 38, buf);

    if (ble_confirmed)
    {
        gfx_text_micro(70, 45, "PACKETS");
        gfx_fmt_int(buf, ble_packets);
        gfx_text_micro(102, 45, buf);
    }

    gfx_hline(0, DISP_W - 1, 53);
    gfx_text_micro(2, 57, ble_confirmed ? "CRC CHECKED, NOT A GUESS"
                                        : "PATTERN MATCH, NOT DECODED");
    display_flush();
}
