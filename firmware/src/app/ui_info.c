#include "battery.h"
#include "display.h"
#include "flash_ext.h"
#include "gfx.h"
#include "power.h"
#include "ui.h"
#include "ui_info.h"

// ---------------------------------------------------------------------------
// Info and diagnostics
// ---------------------------------------------------------------------------

void ui_info(uint32_t uptime_s, uint32_t sweeps, uint16_t history_rows)
{
    char buf[20];
    display_clear();
    ui_title("INFO");

    gfx_text_micro(2, 12, "RESET");
    gfx_text_micro(34, 12, power_reset_reason_name());

    gfx_text_micro(2, 21, "UPTIME");
    gfx_fmt_int(buf, (int)uptime_s);
    gfx_text_micro(46, 21, buf);
    gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 21, "S");

    gfx_text_micro(2, 28, "SWEEPS");
    gfx_fmt_int(buf, (int)sweeps);
    gfx_text_micro(46, 28, buf);

    gfx_text_micro(2, 35, "HISTORY");
    gfx_fmt_int(buf, history_rows);
    gfx_text_micro(46, 35, buf);
    gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 35, "ROWS");

    gfx_text_micro(2, 42, "BATTERY");
    gfx_fmt_fixed(buf, g_battery.mv, 3);
    gfx_text_micro(46, 42, buf);
    gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 42, "V");
    gfx_fmt_int(buf, g_battery.percent);
    gfx_text_micro(78, 42, buf);
    gfx_text_micro(78 + gfx_text_micro_width(buf) + 1, 42, "PCT");

    // Raw numbers, so a gauge that reads wrong can be calibrated against a
    // multimeter with the BATT CAL setting
    gfx_text_micro(2, 49, "ADC RAW");
    gfx_fmt_int(buf, g_battery.raw_adc);
    gfx_text_micro(46, 49, buf);
    gfx_fmt_fixed(buf, g_battery.mv_raw, 3);
    gfx_text_micro(70, 49, buf);
    gfx_text_micro(70 + gfx_text_micro_width(buf) + 2, 49, "V");

    gfx_text_micro(2, 56, "FLASH");
    if (flash_ext_present())
    {
        gfx_fmt_int(buf, (int)(flash_ext_size() / 1024));
        gfx_text_micro(46, 56, buf);
        gfx_text_micro(46 + gfx_text_micro_width(buf) + 2, 56, "KB");
    }
    else
    {
        gfx_text_micro(46, 56, "NONE");
    }

    gfx_text_micro(80, 56, "ATC1441");
    display_flush();
}
