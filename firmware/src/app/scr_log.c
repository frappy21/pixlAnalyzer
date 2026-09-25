/**
 * LOG screen: the NOR event history browser (radar signals, hunt alerts,
 * sentry alarms), and its erase. Rows are the newest events; a long MID
 * hold erases everything, with a message first.
 *
 * Only shown content: the store is written by the radar and the sentry
 * with the user's consent (Settings: NOR log).
 */
#include <string.h>

#include "app.h"
#include "buttons.h"
#include "gfx.h"
#include "log_store.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"

#define LOG_ROWS 6
#define PAGE 6

static uint8_t m_sel;      // selection within the page
static uint32_t m_top;     // newest shown event index, walking down

static void log_enter(void)
{
    m_sel = 0;
    m_top = 0;
}

static void log_draw(void)
{
    char buf[10];
    uint32_t n = log_store_count();

    display_clear();
    ui_title("LOG");

    if (!log_store_enabled())
    {
        gfx_text_micro(2, 24, "NO NOR FLASH FOUND");
        display_flush();
        return;
    }
    if (!g_settings.log_consent)
    {
        gfx_text_micro(2, 22, "CONSENT IS OFF");
        gfx_text_micro(2, 34, "SETTINGS: TOOLS: NOR LOG");
        display_flush();
        return;
    }
    if (!n)
    {
        gfx_text_micro(2, 24, "EMPTY");
        gfx_text_micro(2, 34, "EVENTS APPEAR FROM THE");
        gfx_text_micro(2, 42, "RADAR, HUNT AND SENTRY");
        display_flush();
        return;
    }

    gfx_fmt_int(buf, (int)n);
    gfx_text_micro(DISP_W - 2 - gfx_text_micro_width(buf), 2, buf);

    // Events, newest first: index n-1 down
    for (uint8_t row = 0; row < LOG_ROWS; row++)
    {
        // m_top counts down from n-1
        if (m_top >= n)
            break;
        uint32_t idx = n - 1 - m_top - row;
        if ((int32_t)idx < 0)
            break;

        log_record_t rec;
        if (!log_store_get(idx, &rec))
            break;

        int y = 10 + row * 8;

        gfx_fmt_int(buf, (int)rec.time_s);
        gfx_text_micro(2, y, buf);

        gfx_text_micro(34, y, log_type_name(rec.type));

        if (rec.freq)
        {
            gfx_fmt_int(buf, (int)rec.freq);
            gfx_text_micro(64, y, buf);
            gfx_text_micro(64 + gfx_text_micro_width(buf) + 1, y, "M");
        }

        if (rec.level)
        {
            gfx_fmt_int(buf, -(int)rec.level);
            gfx_text_micro(DISP_W - 14, y, buf);
            gfx_text_micro(DISP_W - 14 + gfx_text_micro_width(buf) + 1, y, "D");
        }

        if (row == m_sel)
            gfx_invert(0, y - 1, DISP_W, 8);
    }

    gfx_text_micro(2, 58, "L/R: SCROLL, HOLD MID: ERASE");
    display_flush();
}

static void log_tick(uint32_t now)
{
    (void)now;
    uint32_t n = log_store_count();

    if (app_left())
    {
        if (m_sel > 0)
            m_sel--;
        else if (m_top + LOG_ROWS < n)
            m_top += PAGE;
        app_redraw();
    }
    if (app_right())
    {
        if (m_sel + 1 < LOG_ROWS && m_top + m_sel + 1 < n)
            m_sel++;
        else if (m_top >= PAGE)
            m_top -= PAGE;
        app_redraw();
    }

    // Erase: a deliberate two second hold
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 2000)
    {
        app_note_input();
        if (log_store_clear())
            ui_message("LOG ERASED", 0, 800);
        else
            ui_message("ERASE FAILED", 0, 800);
        m_top = 0;
        m_sel = 0;
        buttons_flush();
        app_redraw();
        return;
    }

    if (app_take_redraw())
        log_draw();
}

const app_screen_t scr_log = {
    .name = "Event log",
    .group = APP_GROUP_TOOLS,
    .enter = log_enter,
    .tick = log_tick,
};
