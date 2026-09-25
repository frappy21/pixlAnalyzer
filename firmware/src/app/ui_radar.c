#include "gfx.h"
#include "ui.h"
#include "ui_radar.h"

// Strip geometry: 84 channels across the 128 px display
#define STRIP_X 2
#define STRIP_Y 10
#define STRIP_H 18
#define SIG_Y 32
#define SIG_ROWS 3

const char *radar_mode_name(uint8_t mode)
{
    switch (mode)
    {
    case 1:
        return "HUNT";
    case 2:
        return "MICROWAVE";
    default:
        return "SCAN";
    }
}

static void draw_strip(const radar_view_t *view)
{
    gfx_hline(STRIP_X - 1, STRIP_X + view->strip_len, STRIP_Y - 1);
    gfx_hline(STRIP_X - 1, STRIP_X + view->strip_len, STRIP_Y + STRIP_H);

    for (uint8_t i = 0; i < view->strip_len && i < 84; i++)
    {
        // Level 0..255 to a column height 0..STRIP_H-2, dB scaled: the
        // strip value is already dB above floor * 8
        uint8_t h = (uint8_t)((uint32_t)view->strip[i] * (STRIP_H - 2) / 255);
        if (h)
            gfx_vline(STRIP_X + i, STRIP_Y + STRIP_H - h, STRIP_Y + STRIP_H - 2);
    }
}

static void draw_signals(const radar_view_t *view)
{
    char buf[8];
    const radar_work_t *w = view->work;
    uint8_t n = radar_signals(w);

    if (!n)
    {
        gfx_text_micro(2, SIG_Y + 4, view->mode == 1 && view->hunt_armed
                                          ? "BASELINE SET: WATCHING"
                                          : "NO PERSISTENT SIGNALS");
        if (view->mode == 1)
            gfx_text_micro(2, SIG_Y + 14, "ANYTHING NEW WILL FLASH");
        return;
    }

    for (uint8_t i = 0; i < SIG_ROWS && i < n; i++)
    {
        const radar_signal_t *s = radar_signal(w, i);
        int y = SIG_Y + i * 9;

        gfx_fmt_int(buf, (int)s->mhz);
        gfx_text_micro(2, y, buf);
        int x = 2 + gfx_text_micro_width(buf) + 2;
        gfx_text_micro(x, y, "M");

        gfx_fmt_int(buf, -(int)s->peak_db);
        gfx_text_micro(24, y, buf);
        gfx_text_micro(24 + gfx_text_micro_width(buf) + 1, y, "DB");

        gfx_fmt_int(buf, (int)s->busy_pct);
        gfx_text_micro(46, y, buf);
        gfx_text_micro(46 + gfx_text_micro_width(buf) + 1, y, "%");

        const char *kind = radar_kind_name((radar_kind_t)s->kind);
        gfx_text_micro(62, y, kind);

        if (s->first_seen)
            gfx_text_micro(DISP_W - 12, y, "NEW!");
        else if (i == view->selected)
            gfx_invert(0, y - 1, DISP_W, 9);
    }

    if (n > SIG_ROWS)
    {
        gfx_fmt_int(buf, (int)n);
        gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), 2, buf);
        gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf) - 6, 2, "+");
    }
}

static void draw_microwave(const radar_view_t *view)
{
    char buf[10];

    // Level as a bar, 0..60 dB
    gfx_text_micro(2, 26, "2450MHZ LEVEL");
    gfx_fmt_int(buf, (int)view->mw_level);
    gfx_text_micro(66, 26, buf);
    gfx_text_micro(66 + gfx_text_micro_width(buf) + 1, 26, "DB");

    gfx_box(2, 36, 124, 8, false, true);
    int w = (int)((uint32_t)(view->mw_level > 60 ? 60 : view->mw_level) * 120 / 60);
    if (w > 0)
        gfx_box(3, 37, w, 6, true, true);

    // The verdict, centred
    gfx_text_micro((DISP_W - gfx_text_micro_width(view->mw_verdict)) / 2, 50, view->mw_verdict);

    // Trend: recent levels as a spark line
    if (view->mw_trend_len > 1)
    {
        for (uint8_t i = 0; i + 1 < view->mw_trend_len; i++)
        {
            int x1 = 2 + (i * 124) / (view->mw_trend_len - 1);
            int x2 = 2 + ((i + 1) * 124) / (view->mw_trend_len - 1);
            int y1 = 62 - (int)((uint32_t)(view->mw_trend[i] > 20 ? 20 : view->mw_trend[i]) * 6 / 20);
            int y2 = 62 - (int)((uint32_t)(view->mw_trend[i + 1] > 20 ? 20 : view->mw_trend[i + 1]) * 6 / 20);
            gfx_hline(x1 < x2 ? x1 : x2, x1 < x2 ? x2 : x1, y1);
            gfx_vline(x2, y1 < y2 ? y1 : y2, y1 < y2 ? y2 : y1);
        }
    }

    gfx_text_micro(2, 12, "NOT A SAFETY CERTIFICATE");
}

void ui_radar(const radar_view_t *view)
{
    char buf[8];

    display_clear();
    ui_title("RADAR");
    ui_battery();

    // Mode in the title row
    const char *mode = radar_mode_name(view->mode);
    gfx_text_micro(50, 2, mode);

    if (view->mode == 2)
    {
        draw_microwave(view);
        display_flush();
        return;
    }

    draw_strip(view);

    // The band edges under the strip
    gfx_text_micro(2, STRIP_Y + STRIP_H + 1, "24");
    gfx_text_micro(DISP_W - 14, STRIP_Y + STRIP_H + 1, "2483");

    draw_signals(view);

    (void)buf;
    display_flush();
}
