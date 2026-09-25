#include <string.h>

#include "nrf_delay.h"

#include "app_config.h"
#include "battery.h"
#include "board_config.h"
#include "buttons.h"
#include "channels.h"
#include "display.h"
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

static void scanner_status_bar(const scanner_view_t *view)
{
    char buf[16];

    // Left: sweeps per second, the honest measure of how much air we see
    gfx_fmt_int(buf, (int)view->sweeps_s);
    int x = 0;
    gfx_text_micro(x, 1, buf);
    x += gfx_text_micro_width(buf) + 3;
    gfx_text_micro(x, 1, "HZ");
    x += gfx_text_micro_width("HZ") + 5;

    // Middle: current tool, or the frozen marker
    gfx_text_micro(x, 1, view->frozen ? "FROZEN" : tool_name(view->tool));

    // Marker readout: frequency and level
    uint8_t idx = spectrum_col_to_chan(view->marker_col);
    uint16_t mhz = scanner_mhz(idx);
    gfx_fmt_int(buf, mhz);
    gfx_text_micro(46, 1, buf);

    uint8_t peak = g_scan[idx].peak;
    if (peak != RSSI_INVALID)
    {
        char *p = buf;
        *p++ = '-';
        gfx_fmt_int(p, peak);
        gfx_text_micro(72, 1, buf);
    }

    // Right edge, x=104 onwards for "+4.12V": clear of the level at x=72
    ui_battery();
}

void ui_scanner(const scanner_view_t *view)
{
    display_clear();

    scanner_status_bar(view);
    spectrum_draw(view->marker_col);
    spectrum_draw_ruler(view->plan, view->marker_col);
    spectrum_draw_waterfall(view->scroll_back);

    // Scroll position indicator on the right edge of the waterfall
    if (view->scroll_back)
    {
        uint16_t rows = spectrum_history_rows();
        if (rows > WATERFALL_ROWS)
        {
            int y = WATERFALL_START +
                    (int)((uint32_t)view->scroll_back * (WATERFALL_ROWS - 1) / rows);
            gfx_vline(DISP_W - 1, y, y + 1);
        }
    }

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

    for (uint8_t i = 0; i < n; i++)
    {
        int y = 11 + i * 8;
        uint16_t mhz = scanner_mhz(idx[i]);

        gfx_fmt_int(buf, mhz);
        gfx_text_micro(2, y, buf);

        const char *label = channels_label(mhz);
        if (label[0])
            gfx_text_micro(26, y, label);

        // Occupancy bar, 0..255 mapped to 60 pixels
        int w = (g_scan[idx[i]].busy * 60) / 255;
        gfx_box(44, y, 62, 5, false, true);
        if (w > 0)
            gfx_box(45, y + 1, w, 3, true, true);

        gfx_fmt_int(buf, (g_scan[idx[i]].busy * 100) / 255);
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
        char *p = buf;
        *p++ = '-';
        gfx_fmt_int(p, rssi);
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
