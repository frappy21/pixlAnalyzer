#include "display.h"
#include "esb_scan.h"
#include "gfx.h"
#include "ui.h"
#include "ui_esb.h"

// ---------------------------------------------------------------------------
// ShockBurst / nRF24 detection
// ---------------------------------------------------------------------------

void ui_esb_list(uint32_t total)
{
    char buf[16];
    display_clear();

    gfx_text(2, 1, "MOUSE/KBD");
    gfx_fmt_int(buf, (int)total);
    gfx_text_micro(80, 2, buf);
    gfx_text_micro(80 + gfx_text_micro_width(buf) + 2, 2, "HITS");
    gfx_hline(0, DISP_W - 1, 9);

    uint8_t n = esb_scan_count();
    if (n == 0)
    {
        gfx_text_micro(2, 20, "NO SHOCKBURST TRAFFIC YET");
        gfx_text_micro(2, 30, "MOVE THE MOUSE OR TYPE");
        gfx_text_micro(2, 40, "WHILE THE SCAN RUNS");
        display_flush();
        return;
    }

    for (uint8_t i = 0; i < n && i < 5; i++)
    {
        const esb_hit_t *h = esb_scan_hit(i);
        int y = 11 + i * 9;

        gfx_fmt_int(buf, h->mhz);
        gfx_text_micro(2, y, buf);

        gfx_fmt_int(buf, h->rate);
        gfx_text_micro(28, y, buf);
        gfx_text_micro(28 + gfx_text_micro_width(buf) + 1, y, "M");

        gfx_fmt_int(buf, h->packets);
        gfx_text_micro(48, y, buf);
        gfx_text_micro(48 + gfx_text_micro_width(buf) + 2, y, "PKT");

        char *q = buf;
        *q++ = '-';
        gfx_fmt_int(q, h->peak_rssi);
        gfx_text_micro(DISP_W - 24, y, buf);
    }

    gfx_hline(0, DISP_W - 1, 55);
    gfx_text_micro(2, 58, "PREAMBLE LOCK, NOT DECODED");
    display_flush();
}
