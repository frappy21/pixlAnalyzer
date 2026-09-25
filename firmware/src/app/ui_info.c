#include "nrf.h"

#include "battery.h"
#include "display.h"
#include "flash_ext.h"
#include "gfx.h"
#include "power.h"
#include "ui.h"
#include "ui_info.h"
#include "version.h"

// Micro font rows below the title
#define ROW(n) (12 + (n) * 7)
#define VALUE_X 40

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

static char *fmt_hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--)
    {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = 0;
    return buf;
}

// h:mm:ss, hours unbounded
static char *fmt_hms(char *buf, uint32_t s)
{
    char *p = gfx_fmt_int(buf, (int)(s / 3600));
    while (*p)
        p++;
    uint32_t m = (s / 60) % 60;
    uint32_t sec = s % 60;
    *p++ = ':';
    *p++ = (char)('0' + m / 10);
    *p++ = (char)('0' + m % 10);
    *p++ = ':';
    *p++ = (char)('0' + sec / 10);
    *p++ = (char)('0' + sec % 10);
    *p = 0;
    return buf;
}

// Label on the left, value in the value column, optional unit after it
static void line(int row, const char *label, const char *value, const char *unit)
{
    gfx_text_micro(2, ROW(row), label);
    gfx_text_micro(VALUE_X, ROW(row), value);
    if (unit)
        gfx_text_micro(VALUE_X + gfx_text_micro_width(value) + 3, ROW(row), unit);
}

static void title(uint8_t page)
{
    static const char *const titles[INFO_PAGE_COUNT] = {"SYSTEM", "POWER", "CRASH"};
    char buf[4] = {(char)('1' + page), '/', (char)('0' + INFO_PAGE_COUNT), 0};

    display_clear();
    ui_title(titles[page]);
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), 2, buf);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static void page_system(const info_view_t *view)
{
    char buf[20];

    gfx_text_micro(2, ROW(0), "FW");
    gfx_text_micro(VALUE_X, ROW(0), g_fw_version);
    gfx_text_micro(VALUE_X + gfx_text_micro_width(g_fw_version) + 4, ROW(0), g_fw_build);

    line(1, "RESET", power_reset_reason_name(), 0);
    line(2, "UPTIME", fmt_hms(buf, view->uptime_s), 0);
    // Since the last power cycle: survives crashes and watchdog resets
    line(3, "RUNTIME", fmt_hms(buf, power_runtime_s()), 0);
    line(4, "CHIP", gfx_fmt_fixed(buf, (int)power_temperature_q2() * 25, 2), "C");

    // Stack: bytes never touched since boot, of the usable size
    gfx_fmt_int(buf, (int)power_stack_unused());
    line(5, "STACK", buf, "FREE OF");
    gfx_fmt_int(buf, (int)power_stack_size());
    gfx_text_micro(96, ROW(5), buf);

    line(6, "RAM FREE", gfx_fmt_int(buf, (int)power_ram_gap()), "B UNUSED");
}

static void page_power(const info_view_t *view)
{
    char buf[20];

    gfx_fmt_fixed(buf, g_battery.mv, 3);
    line(0, "BATTERY", buf, "V");
    gfx_fmt_int(buf, g_battery.percent);
    gfx_text_micro(84, ROW(0), buf);
    gfx_text_micro(84 + gfx_text_micro_width(buf) + 1, ROW(0), "PCT");

    // Raw numbers, so a gauge that reads wrong can be calibrated against a
    // multimeter with the BATT CAL setting
    line(1, "ADC RAW", gfx_fmt_int(buf, g_battery.raw_adc), 0);
    gfx_fmt_fixed(buf, g_battery.mv_raw, 3);
    gfx_text_micro(70, ROW(1), buf);
    gfx_text_micro(70 + gfx_text_micro_width(buf) + 2, ROW(1), "V");

    line(2, "SWEEPS", gfx_fmt_int(buf, (int)view->sweeps), 0);
    line(3, "HISTORY", gfx_fmt_int(buf, view->history_rows), "ROWS");

    if (flash_ext_present())
        line(4, "FLASH", gfx_fmt_int(buf, (int)(flash_ext_size() / 1024)), "KB");
    else
        line(4, "FLASH", "NONE", 0);

    gfx_text_micro(2, ROW(6), "ATC1441");
}

// The most telling status bit, so the record reads without the ARM manual
static const char *crash_cause(const power_crash_t *c)
{
    static const struct
    {
        uint32_t mask;
        const char *name;
    } bits[] = {
        {SCB_CFSR_MSTKERR_Msk, "STACKING"},
        {SCB_CFSR_MUNSTKERR_Msk, "UNSTACKING"},
        {SCB_CFSR_DACCVIOL_Msk, "MPU DATA ACCESS"},
        {SCB_CFSR_IACCVIOL_Msk, "MPU EXECUTE"},
        {SCB_CFSR_STKERR_Msk, "BUS STACKING"},
        {SCB_CFSR_PRECISERR_Msk, "BUS DATA"},
        {SCB_CFSR_IMPRECISERR_Msk, "IMPRECISE BUS"},
        {SCB_CFSR_IBUSERR_Msk, "BUS FETCH"},
        {SCB_CFSR_UNDEFINSTR_Msk, "UNDEFINED INSTR"},
        {SCB_CFSR_INVSTATE_Msk, "INVALID STATE"},
        {SCB_CFSR_INVPC_Msk, "INVALID PC"},
        {SCB_CFSR_NOCP_Msk, "NO COPROCESSOR"},
        {SCB_CFSR_UNALIGNED_Msk, "UNALIGNED"},
        {SCB_CFSR_DIVBYZERO_Msk, "DIVIDE BY ZERO"},
    };

    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        if (c->cfsr & bits[i].mask)
            return bits[i].name;
    }
    if (c->hfsr & SCB_HFSR_VECTTBL_Msk)
        return "VECTOR READ";
    return "UNKNOWN";
}

void ui_info_crash_lines(const power_crash_t *c, int y)
{
    char buf[12];

    gfx_text_micro(2, y, c->vector == 4 ? "MEMMANAGE" : "HARDFAULT");
    gfx_text_micro(44, y, c->overflow ? "STACK OVERFLOW" : crash_cause(c));
    if (c->count > 1)
    {
        char *p = buf;
        *p++ = 'X';
        gfx_fmt_int(p, c->count);
        gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), y, buf);
    }

    gfx_text_micro(2, y + 7, "PC");
    gfx_text_micro(18, y + 7, fmt_hex8(buf, c->pc));
    gfx_text_micro(64, y + 7, "LR");
    gfx_text_micro(80, y + 7, fmt_hex8(buf, c->lr));

    gfx_text_micro(2, y + 14, "CFSR");
    gfx_text_micro(22, y + 14, fmt_hex8(buf, c->cfsr));
    gfx_text_micro(64, y + 14, "HFSR");
    gfx_text_micro(84, y + 14, fmt_hex8(buf, c->hfsr));

    // The fault address when the core latched one, the status word otherwise
    if (c->cfsr & SCB_CFSR_BFARVALID_Msk)
    {
        gfx_text_micro(2, y + 21, "BFAR");
        gfx_text_micro(22, y + 21, fmt_hex8(buf, c->bfar));
    }
    else if (c->cfsr & SCB_CFSR_MMARVALID_Msk)
    {
        gfx_text_micro(2, y + 21, "MMAR");
        gfx_text_micro(22, y + 21, fmt_hex8(buf, c->mmfar));
    }
    else
    {
        gfx_text_micro(2, y + 21, "PSR");
        gfx_text_micro(22, y + 21, fmt_hex8(buf, c->xpsr));
    }
    gfx_text_micro(64, y + 21, "SP");
    gfx_text_micro(84, y + 21, fmt_hex8(buf, c->sp));
}

static void page_crash(const info_view_t *view)
{
    power_crash_t crash;

    if (power_crash_get(&crash))
        ui_info_crash_lines(&crash, ROW(0));
    else
        gfx_text_micro(2, ROW(0), "NO CRASH SINCE POWER ON");

    // Checks the catcher on hardware: the device resets and shows the record
    gfx_hline(0, DISP_W - 1, ROW(4) + 1);
    gfx_text_micro(2, ROW(5) - 1, "TEST");
    gfx_text_micro(22, ROW(5) - 1, view->test_overflow ? "STACK OVERFLOW" : "UNDEFINED INSTR");
    gfx_text_micro(2, ROW(6) - 1, "CLICK: TYPE  HOLD MID: CRASH");
}

void ui_info(const info_view_t *view)
{
    title(view->page);

    switch (view->page)
    {
    case INFO_PAGE_POWER:
        page_power(view);
        break;
    case INFO_PAGE_CRASH:
        page_crash(view);
        break;
    case INFO_PAGE_SYSTEM:
    default:
        page_system(view);
        break;
    }

    display_flush();
}
