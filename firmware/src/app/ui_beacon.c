#include "ble_beacon.h"
#include "display.h"
#include "gfx.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_beacon.h"

enum
{
    ROW_BACK = 0,
    ROW_TYPE,
    ROW_PWR,
    ROW_INTV,
    ROW_COUNT
};

void ui_beacon_config(uint8_t type, uint8_t sel, uint8_t power, uint16_t interval_ms, bool edit)
{
    static char intv_str[8];

    display_clear();
    ui_title(edit ? "TX: EDIT" : "BEACON TX");

    const char *rows[ROW_COUNT] = {"Back", "Type", "Power", "Every"};
    const char *vals[ROW_COUNT];

    vals[ROW_BACK] = "";
    vals[ROW_TYPE] = ble_beacon_name(type);
    vals[ROW_PWR] = tx_power_name(power);
    gfx_fmt_int(intv_str, (int)interval_ms);
    vals[ROW_INTV] = intv_str;

    for (uint8_t row = 0; row < ROW_COUNT; row++)
    {
        int y = 12 + row * 9;
        gfx_text(4, y, rows[row]);
        if (vals[row][0])
        {
            int w = gfx_text_width(vals[row]);
            gfx_text(DISP_W - 3 - w, y, vals[row]);
        }
        if (row == sel)
            gfx_invert(0, y - 1, DISP_W, 9);
    }

    gfx_hline(0, DISP_W - 1, 49);
    gfx_text_micro(2, 51, ble_beacon_desc(type));
    gfx_text_micro(2, 58, "OWN LINKS. HOLD MID 1S");
    display_flush();
}

void ui_beacon_active(uint8_t type, uint32_t sent, uint32_t remaining_ms)
{
    char buf[16];

    display_clear();

    gfx_box(0, 0, DISP_W, 12, true, true);
    gfx_text(28, 2, "BEACON ON");
    gfx_invert(0, 0, DISP_W, 12);

    gfx_text(2, 20, ble_beacon_name(type));

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
