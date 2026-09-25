/**
 * ANT+ sensor decoder screen: passive broadcast receiver on 2457 MHz.
 *
 * Shows a list of up to 8 ANT+ channels, with HR / power decoded for
 * known profiles. Other channels show the page number and raw data bytes.
 *
 * Short MID: toggle between list view and raw frame view.
 */
#include <string.h>

#include "ant_rx.h"
#include "app.h"
#include "display.h"
#include "gfx.h"
#include "scanner.h"
#include "screens.h"
#include "systime.h"
#include "ui.h"

// Arena share: ANT work struct is small, lives in the upper half so it
// does not conflict with the spectrum screen using the lower half.
#define ANT_ARENA_OFF  8192
#define ANT_ARENA_SIZE 512

#define ANT_SLICE_MS   100
#define ANT_REDRAW_MS  500
#define ANT_STALE_MS   5000  // dim a device after 5 s of silence

static bool m_raw_view;
static uint8_t m_sel;
static uint32_t m_redraw_ms;

static void ant_enter(void)
{
    ant_rx_init(g_app_arena + ANT_ARENA_OFF, ANT_ARENA_SIZE);
    m_raw_view = false;
    m_sel = 0;
    m_redraw_ms = 0;
}

static void draw_list(uint32_t now)
{
    char buf[24];
    uint8_t n = ant_rx_count();

    display_clear();
    gfx_text(2, 1, "ANT+");
    gfx_text_micro(38, 2, "2457 MHz");
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    if (n == 0)
    {
        gfx_text_micro(2, 16, "SEARCHING...");
        gfx_text_micro(2, 26, "NO ANT+ DEVICES");
        gfx_text_micro(2, 36, "CHANNEL 57 (2457 MHZ)");
        display_flush();
        return;
    }

    // Up to 5 rows at 11 px each (rows 11..65)
    uint8_t rows = n < 5 ? n : 5;
    for (uint8_t i = 0; i < rows; i++)
    {
        const ant_dev_t *d = ant_rx_device(i);
        if (!d)
            break;

        int y = 11 + i * 11;
        bool stale = (now - d->last_ms) > ANT_STALE_MS;

        // Channel number
        char *p = gfx_fmt_int(buf, d->channel);
        gfx_text_micro(2, y, "CH");
        gfx_text_micro(10, y, buf);
        (void)p;

        // Profile + value
        if (d->profile == ANT_PROF_HR)
        {
            gfx_text_micro(28, y, stale ? "HR:  ?" : "HR:");
            if (!stale)
            {
                gfx_fmt_int(buf, d->hr_bpm);
                gfx_text_micro(42, y, buf);
                gfx_text_micro(42 + gfx_text_micro_width(buf) + 2, y, "BPM");
            }
        }
        else if (d->profile == ANT_PROF_POWER)
        {
            gfx_text_micro(28, y, stale ? "PWR: ?" : "PWR:");
            if (!stale)
            {
                gfx_fmt_int(buf, d->power_w);
                gfx_text_micro(44, y, buf);
                gfx_text_micro(44 + gfx_text_micro_width(buf) + 2, y, "W");
            }
        }
        else
        {
            gfx_text_micro(28, y, "PG");
            gfx_fmt_int(buf, d->page);
            gfx_text_micro(36, y, buf);
        }

        // RSSI
        if (d->rssi)
        {
            buf[0] = '-';
            gfx_fmt_int(buf + 1, d->rssi);
            gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), y, buf);
        }

        if (i == m_sel)
            gfx_invert(0, y - 1, DISP_W, 9);
    }

    if (n > 5)
    {
        gfx_fmt_int(buf, n - 5);
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf) - gfx_text_micro_width("+"),
                       11 + 4 * 11, "+");
        gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 11 + 4 * 11, buf);
    }

    display_flush();
}

static void draw_raw(void)
{
    char buf[24];
    uint8_t n = ant_rx_count();

    display_clear();
    gfx_text(2, 1, "ANT+");
    gfx_text_micro(38, 2, "RAW");
    ui_battery();
    gfx_hline(0, DISP_W - 1, 9);

    if (n == 0 || m_sel >= n)
    {
        gfx_text_micro(2, 16, "NO DATA");
        display_flush();
        return;
    }

    const ant_dev_t *d = ant_rx_device(m_sel);
    if (!d)
    {
        display_flush();
        return;
    }

    // Header: channel, page, frame count
    char *p = gfx_fmt_int(buf, d->channel);
    gfx_text_micro(2, 11, "CH");
    gfx_text_micro(10, 11, buf);
    (void)p;

    gfx_text_micro(28, 11, "PG");
    gfx_fmt_int(buf, d->page);
    gfx_text_micro(36, 11, buf);

    gfx_fmt_int(buf, (int)d->frames);
    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf) - gfx_text_micro_width("FRM"),
                   11, "FRM");
    gfx_text_micro(DISP_W - 1 - gfx_text_micro_width(buf), 11, buf);

    gfx_hline(0, DISP_W - 1, 20);

    // Raw bytes: two rows of 4
    static const char k_hex[] = "0123456789ABCDEF";
    for (int row = 0; row < 2; row++)
    {
        int y = 23 + row * 10;
        char hex[3];
        hex[2] = '\0';
        for (int col = 0; col < 4; col++)
        {
            int k = row * 4 + col;
            hex[0] = k_hex[d->raw[k] >> 4];
            hex[1] = k_hex[d->raw[k] & 15];
            gfx_text_micro(2 + col * 30, y, hex);
            // Label underneath
            char label[4];
            label[0] = 'D';
            gfx_fmt_int(label + 1, k);
            gfx_text_micro(2 + col * 30, y + 6, label);
        }
    }

    gfx_hline(0, DISP_W - 1, 44);
    gfx_text_micro(2, 46, "MID:LIST  L/R:CHANNEL");

    display_flush();
}

static void ant_tick(uint32_t now)
{
    ant_rx_run(ANT_SLICE_MS);

    uint8_t n = ant_rx_count();

    if (app_ok())
    {
        m_raw_view = !m_raw_view;
        app_redraw();
        return;
    }
    if (app_left() && m_sel > 0)
    {
        m_sel--;
        app_redraw();
    }
    if (app_right() && m_sel + 1 < n)
    {
        m_sel++;
        app_redraw();
    }

    if (now - m_redraw_ms >= ANT_REDRAW_MS)
    {
        m_redraw_ms = now;
        app_redraw();
    }

    if (app_take_redraw())
    {
        if (m_raw_view)
            draw_raw();
        else
            draw_list(now);
    }
}

const app_screen_t scr_ant = {
    .name = "ANT+",
    .group = APP_GROUP_RECEIVE,
    .enter = ant_enter,
    .tick = ant_tick,
    .busy = true,
};
