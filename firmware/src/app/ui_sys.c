#include "buttons.h"
#include "display.h"
#include "gfx.h"
#include "power.h"
#include "systime.h"
#include "ui.h"
#include "ui_info.h"
#include "ui_sys.h"
#include "version.h"

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

void ui_sys_boot_overlay(void)
{
    // Inside the frame ui_boot_screen() draws at (2,2), left and right of
    // its "2.4GHz" line
    gfx_text_micro(6, 6, g_fw_version);
    gfx_text_micro(DISP_W - 6 - gfx_text_micro_width(g_fw_build), 6, g_fw_build);
}

void ui_sys_crash_report(uint32_t timeout_ms)
{
    power_crash_t crash;
    if (!power_crash_get(&crash))
        return;

    display_clear();
    ui_title("CRASHED");
    ui_info_crash_lines(&crash, 12);
    gfx_hline(0, DISP_W - 1, 42);
    gfx_text_micro(2, 45, "KEPT UNDER INFO - CRASH");
    gfx_text_micro(2, 52, "UNTIL THE NEXT POWER CYCLE");
    gfx_text_micro(2, 58, "PRESS A BUTTON");
    display_flush();

    // Whatever is still held from before must not dismiss the report
    buttons_flush();
    uint32_t start = systime_ms();
    while (systime_ms() - start < timeout_ms)
    {
        buttons_poll();
        if (buttons_pressed(BTN_LEFT) || buttons_pressed(BTN_MID) || buttons_pressed(BTN_RIGHT))
            break;
        systime_idle(20);
    }
    buttons_flush();
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

#define HELP_TOP 12
#define HELP_ROW_H 7
#define HELP_ROWS ((DISP_H - HELP_TOP) / HELP_ROW_H)

// A line starting with a space is an entry, anything else a heading. The
// micro font has capitals, digits and - . / + : ? only.
static const char *const help_text[] = {
    "MAIN SCREENS",
    " SPECTRUM  WIFI  BLE  RC",
    " HOLD L / R   PREV/NEXT SCREEN",
    " HOLD MID     OPEN THE MENU",
    " CLICKS       ACT ON THE SCREEN",
    "SPECTRUM",
    " CLICK MID    NEXT TOOL",
    " L / R        ADJUST THE TOOL",
    " MARK SPAN WFALL SCROLL",
    "MENUS AND OTHER SCREENS",
    " L / R        MOVE OR CHANGE",
    " CLICK MID    SELECT OR EDIT",
    " HOLD LEFT    BACK",
    "SENTRY - MENU: TOOLS",
    " CLICK        WAKE/ACKNOWLEDGE",
    " HOLD LEFT    LEAVE",
    "POWER",
    " MENU: SLEEP  MID WAKES UP",
    " HOLD L OR R AT START: DFU",
};

#define HELP_LINES (sizeof(help_text) / sizeof(help_text[0]))

uint8_t ui_help_lines(void)
{
    return (uint8_t)HELP_LINES;
}

uint8_t ui_help_rows(void)
{
    return (uint8_t)HELP_ROWS;
}

void ui_help(uint8_t first)
{
    display_clear();
    ui_title("HELP");

    for (uint8_t row = 0; row < HELP_ROWS && first + row < HELP_LINES; row++)
    {
        const char *text = help_text[first + row];
        int y = HELP_TOP + row * HELP_ROW_H;
        if (text[0] == ' ')
        {
            gfx_text_micro(4, y, text + 1);
        }
        else
        {
            // Heading: inverted bar across the line
            gfx_text_micro(2, y, text);
            gfx_invert(0, y - 1, DISP_W - 3, HELP_ROW_H);
        }
    }

    // Scrollbar
    int track = DISP_H - HELP_TOP;
    int h = (track * HELP_ROWS) / (int)HELP_LINES;
    int y = HELP_TOP + (track * first) / (int)HELP_LINES;
    gfx_box(DISP_W - 1, y, 1, h, true, true);

    display_flush();
}

// ---------------------------------------------------------------------------
// Sentry
// ---------------------------------------------------------------------------

void ui_sentry(const sentry_view_t *view)
{
    char buf[12];
    display_clear();

    // Few lit pixels while armed: this screen may stay up for hours
    gfx_text_micro(2, 2, "SENTRY");
    const char *state = view->alert ? "ALERT" : "ARMED";
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(state), 2, state);

    gfx_text_micro(2, 12, "OVER FLOOR");
    gfx_fmt_int(buf, view->threshold);
    gfx_text_micro(46, 12, buf);
    gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 12, "DB");

    gfx_text_micro(2, 19, "SWEEP EVERY");
    gfx_fmt_int(buf, view->period_ms);
    gfx_text_micro(46, 19, buf);
    gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 19, "MS");

    gfx_text_micro(2, 26, "SWEEPS");
    gfx_text_micro(46, 26, gfx_fmt_int(buf, (int)view->sweeps));

    gfx_text_micro(2, 33, "ALERTS");
    gfx_text_micro(46, 33, gfx_fmt_int(buf, (int)view->alerts));

    if (view->last_mhz)
    {
        gfx_text_micro(2, 40, "LAST");
        gfx_fmt_int(buf, view->last_mhz);
        gfx_text_micro(46, 40, buf);
        int x = 46 + gfx_text_micro_width(buf) + 2;
        gfx_text_micro(x, 40, "MHZ +");
        x += gfx_text_micro_width("MHZ +") + 1;
        gfx_fmt_int(buf, view->last_db);
        gfx_text_micro(x, 40, buf);
        gfx_text_micro(x + gfx_text_micro_width(buf) + 2, 40, "DB");

        gfx_fmt_int(buf, (int)view->last_age_s);
        gfx_text_micro(46, 47, buf);
        gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 47, "S AGO");
    }

    gfx_text_micro(2, 58, view->alert ? "CLICK: ACKNOWLEDGE" : "CLICK: WAKE  HOLD L: LEAVE");
    display_flush();
}

void ui_sentry_banner(uint16_t mhz, uint8_t db)
{
    char buf[12];
    char line[20];

    // "2437 +23DB"
    char *p = line;
    for (const char *s = gfx_fmt_int(buf, mhz); *s;)
        *p++ = *s++;
    *p++ = ' ';
    *p++ = '+';
    for (const char *s = gfx_fmt_int(buf, db); *s;)
        *p++ = *s++;
    *p++ = 'D';
    *p++ = 'B';
    *p = 0;

    int w = gfx_text_width("ACTIVITY");
    if (gfx_text_width(line) > w)
        w = gfx_text_width(line);
    w += 10;
    int x = (DISP_W - w) / 2;

    gfx_box(x, 14, w, 24, true, false);
    gfx_box(x, 14, w, 24, false, true);
    gfx_text(x + (w - gfx_text_width("ACTIVITY")) / 2, 17, "ACTIVITY");
    gfx_text(x + (w - gfx_text_width(line)) / 2, 27, line);
}
