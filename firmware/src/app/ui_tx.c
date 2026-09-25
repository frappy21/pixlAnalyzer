#include "display.h"
#include "gfx.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_tx.h"

// ---------------------------------------------------------------------------
// Transmitter test
// ---------------------------------------------------------------------------

void ui_tx_confirm(uint16_t mhz, uint8_t power)
{
    char buf[16];
    display_clear();
    ui_title("TX TEST");

    gfx_text_micro(2, 12, "TRANSMITS A CARRIER.");
    gfx_text_micro(2, 20, "CHECK YOUR LOCAL RULES");
    gfx_text_micro(2, 28, "BEFORE ENABLING IT.");

    gfx_text(2, 38, "FREQ");
    gfx_fmt_int(buf, mhz);
    gfx_text(40, 38, buf);

    gfx_text(2, 48, "PWR");
    gfx_text(40, 48, tx_power_name(power));

    gfx_text_micro(2, 58, "MID HOLD 1S TO START");
    display_flush();
}

void ui_tx_active(uint16_t mhz, uint8_t power, uint32_t remaining_ms)
{
    char buf[16];
    display_clear();

    gfx_box(0, 0, DISP_W, 12, true, true);
    gfx_text(30, 2, "TX ACTIVE");
    gfx_invert(0, 0, DISP_W, 12);

    gfx_fmt_int(buf, mhz);
    gfx_text(20, 20, buf);
    gfx_text(20 + gfx_text_width(buf) + 4, 20, "MHz");
    gfx_text(20, 32, tx_power_name(power));

    gfx_text_micro(2, 46, "STOPS IN");
    gfx_fmt_int(buf, (int)(remaining_ms / 1000));
    gfx_text_micro(44, 46, buf);
    gfx_text_micro(44 + gfx_text_micro_width(buf) + 2, 46, "S");

    gfx_text_micro(2, 56, "ANY BUTTON STOPS NOW");
    display_flush();
}
