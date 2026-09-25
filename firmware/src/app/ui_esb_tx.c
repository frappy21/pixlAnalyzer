#include "display.h"
#include "gfx.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_esb_tx.h"

void ui_esb_tx_active(uint8_t mode, uint16_t mhz, uint8_t rate, uint32_t sent, uint32_t remaining_ms)
{
    char buf[16];

    display_clear();

    gfx_box(0, 0, DISP_W, 12, true, true);
    gfx_text(28, 2, "ESB TX ACTIVE");
    gfx_invert(0, 0, DISP_W, 12);

    gfx_text(2, 20, mode ? "INJECT" : "REPLAY");
    gfx_fmt_int(buf, mhz);
    gfx_text(42, 20, buf);
    gfx_text(42 + gfx_text_width(buf) + 4, 20, "MHz");
    gfx_fmt_int(buf, rate);
    gfx_text(90, 20, buf);
    gfx_text(90 + gfx_text_width(buf) + 2, 20, "M");

    gfx_text(2, 32, "SENT");
    gfx_fmt_int(buf, (int)sent);
    gfx_text(34, 32, buf);

    gfx_text_micro(2, 46, "STOPS IN");
    gfx_fmt_int(buf, (int)(remaining_ms / 1000));
    gfx_text_micro(44, 46, buf);
    gfx_text_micro(44 + gfx_text_micro_width(buf) + 2, 46, "S");

    gfx_text_micro(2, 56, "LAB USE ONLY. ANY BTN STOPS");
    display_flush();
}
