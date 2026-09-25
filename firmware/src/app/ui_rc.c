#include <string.h>

#include "gfx.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_rc.h"

// Layout: title row, four stick rows, one status row, 64px tall
#define DASH_ROW_Y(i) (11 + (i) * 9)

// ---------------------------------------------------------------------------
// Stick rows
// ---------------------------------------------------------------------------

void ui_rc_stick_row(int y, const char *name, int8_t value, bool signed_stick)
{
    char buf[8];

    gfx_text_micro(2, y, name);

    int bar_x = 26;
    int bar_w = 78;
    gfx_hline(bar_x, bar_x + bar_w - 1, y + 4);

    if (signed_stick)
    {
        // A bar around the centre: the left half is negative
        int centre = bar_x + bar_w / 2;
        gfx_vline(centre, y + 1, y + 6);

        int len = (int)((bar_w / 2 - 1) * (value < 0 ? -value : value)) / 100;
        if (len > bar_w / 2 - 1)
            len = bar_w / 2 - 1;
        if (len > 0)
        {
            int x0 = value < 0 ? centre - len : centre + 1;
            for (int x = x0; x < x0 + len; x++)
                gfx_vline(x, y + 2, y + 6);
        }
    }
    else
    {
        // Throttle: filled from the left
        int len = (int)((uint32_t)bar_w * (uint8_t)value) / 255;
        for (int x = bar_x; x < bar_x + len; x++)
            gfx_vline(x, y + 2, y + 6);
    }

    // The numeric value, right aligned
    gfx_fmt_int(buf, value);
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), y, buf);
}

static void flag_text(char *buf, uint8_t flags)
{
    // Up to four letters of what is active, in a fixed order
    uint8_t n = 0;
    if (flags & RC_FLAG_FLIP)
        buf[n++] = 'F';
    if (flags & RC_FLAG_RTH)
        buf[n++] = 'R';
    if (flags & RC_FLAG_HEADLESS)
        buf[n++] = 'H';
    if (flags & RC_FLAG_VIDEO)
        buf[n++] = 'V';
    if (flags & RC_FLAG_PHOTO)
        buf[n++] = 'P';
    if (flags & RC_FLAG_RATE)
        buf[n++] = 'X';
    if (flags & RC_FLAG_LED)
        buf[n++] = 'L';
    if (n == 0)
    {
        buf[n++] = '-';
    }
    buf[n] = 0;
}

// ---------------------------------------------------------------------------
// Dash
// ---------------------------------------------------------------------------

void ui_rc_dash(const rc_dash_view_t *view)
{
    char buf[10];

    display_clear();
    ui_title("RC DASH");
    ui_battery();

    if (!view->locked && view->pps == 0 && !view->reacquires)
    {
        gfx_text_micro(2, 24, "NO RC LINK YET");
        gfx_text_micro(2, 34, "POWER A 2.4GHZ TOY UP");
        gfx_text_micro(2, 44, "KNOWN FAMILIES + GENERIC");
        display_flush();
        return;
    }

    if (view->bind)
    {
        // A bind packet: show what the link is negotiating, the sticks are
        // meaningless during bind
        gfx_text_micro(2, 22, "BIND PHASE");
        gfx_text_micro(2, 32, "THE TOY IS PAIRING:");
        gfx_text_micro(2, 42, "DATA LINK FOLLOWS THE ID");
        gfx_text_micro(2, 56, "STAY ON, IT LOCKS ITSELF");
        display_flush();
        return;
    }

    // Protocol name top right, channel top left under the title
    gfx_text_micro(2, 2, "");
    gfx_fmt_int(buf, (int)view->mhz);
    gfx_text_micro(2, 2, buf);
    gfx_text_micro(2 + gfx_text_micro_width(buf) + 2, 2, "MHZ");

    const char *proto = view->proto >= 0 ? rc_proto_name(view->proto) : "GENERIC";
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(proto), 2, proto);

    if (view->proto >= 0)
    {
        ui_rc_stick_row(DASH_ROW_Y(0), "THR", (int8_t)view->sticks.throttle, false);
        ui_rc_stick_row(DASH_ROW_Y(1), "YAW", view->sticks.yaw, true);
        ui_rc_stick_row(DASH_ROW_Y(2), "PIT", view->sticks.pitch, true);
        ui_rc_stick_row(DASH_ROW_Y(3), "ROL", view->sticks.roll, true);

        // Status row: flags, RSSI, rate
        char fl[8];
        flag_text(fl, view->flags);
        gfx_text_micro(2, 56, fl);

        gfx_fmt_int(buf, -(int)view->rssi);
        gfx_text_micro(28, 56, buf);
        gfx_text_micro(28 + gfx_text_micro_width(buf) + 1, 56, "DB");

        gfx_fmt_int(buf, (int)view->pps);
        gfx_text_micro(52, 56, buf);
        gfx_text_micro(52 + gfx_text_micro_width(buf) + 1, 56, "P/S");

        if (view->reacquires)
        {
            gfx_fmt_int(buf, (int)view->reacquires);
            gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf) - 4, 56, "RQ");
            gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), 56, buf);
        }
    }
    else
    {
        // Generic view: the moving payload bytes as bars around their
        // observed centre
        uint8_t shown = 0;
        for (uint8_t i = 0; i < RC_TRACK_MAX && shown < 4; i++)
        {
            if (!(view->live_mask & (1u << i)))
                continue;

            char name[4] = {'B', '0' + (char)(i / 10), '0' + (char)(i % 10), 0};
            int8_t v = (int8_t)((int16_t)view->value[i] - (int16_t)view->centre[i]);
            ui_rc_stick_row(DASH_ROW_Y(shown), name, v, true);
            shown++;
        }
        if (!shown)
            gfx_text_micro(2, DASH_ROW_Y(0), "WAITING FOR MOVEMENT");

        gfx_text_micro(2, 56, "UNKNOWN PROTOCOL: RAW BYTES");
    }

    display_flush();
}

// ---------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------

void ui_rc_tx(const rc_tx_view_t *view)
{
    char buf[10];

    display_clear();
    ui_title("RC EMULATOR");
    ui_battery();

    if (!view->has_capture)
    {
        gfx_text_micro(2, 24, "CAPTURE A CONTROL PACKET");
        gfx_text_micro(2, 34, "IN RC DASH OR SNIFF FIRST");
        gfx_text_micro(2, 44, "(MID ON THE DASH CAPTURES)");
        display_flush();
        return;
    }

    const char *proto = view->proto >= 0 ? rc_proto_name(view->proto) : "GENERIC";

    if (view->running)
    {
        gfx_text_micro(2, 12, "TRANSMITTING");
        gfx_text_micro(2, 22, proto);
        gfx_fmt_int(buf, (int)view->remaining_s);
        gfx_text_micro(2, 34, buf);
        gfx_text_micro(2 + gfx_text_micro_width(buf) + 2, 34, "S LEFT");
        gfx_text_micro(2, 46, "ANY BUTTON STOPS");
        display_flush();
        return;
    }

    // Six rows: the four sticks, power, burst. Values right aligned, the
    // selected row inverted.
    static const char *const labels[6] = {"THR", "YAW", "PIT", "ROL", "PWR", "BURST"};

    for (uint8_t i = 0; i < 6; i++)
    {
        int y = 10 + i * 8;

        const char *value_str = 0;
        int value_int = 0;

        switch (i)
        {
        case 0:
            value_int = (int)view->sticks.throttle;
            break;
        case 1:
            value_int = view->sticks.yaw;
            break;
        case 2:
            value_int = view->sticks.pitch;
            break;
        case 3:
            value_int = view->sticks.roll;
            break;
        case 4:
            value_str = tx_power_name(view->power);
            break;
        default:
            if (view->count == 0)
                value_str = "LOOP 10S";
            else
                value_int = view->count;
            break;
        }

        gfx_text_micro(2, y, labels[i]);

        if (value_str)
            gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(value_str), y, value_str);
        else
        {
            gfx_fmt_int(buf, value_int);
            gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), y, buf);
        }

        if (i == view->selected)
            gfx_invert(0, y - 1, DISP_W, 8);
    }

    gfx_text_micro(2, 58, "HOLD MID TO ARM, OWN TOY ONLY");
    display_flush();
}
