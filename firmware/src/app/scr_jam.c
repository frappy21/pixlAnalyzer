/**
 * JAMMER screen: the lab interference transmitter (noise / sweep / WiFi
 * channel), for immunity testing of self owned receivers.
 *
 * Rows: mode, channel (or WiFi channel), rate, power. Starting is
 * deliberate: a one second hold of MID. While it runs, any button stops
 * it, it stops itself after JAM_MAX_MS, and every exit path stops it. The
 * screen says what it is for and so does the confirm page.
 */
#include <string.h>

#include "app.h"
#include "buttons.h"
#include "gfx.h"
#include "jam.h"
#include "screens.h"
#include "tx_test.h"
#include "ui.h"

enum
{
    ROW_BACK = 0,
    ROW_MODE,
    ROW_CH,
    ROW_RATE,
    ROW_PWR,
    ROW_COUNT
};

static bool m_active;
static uint8_t m_row_sel;
static uint8_t m_mode;          // jam_mode_t
static uint16_t m_mhz = 2440;
static uint8_t m_wifi_chan = 6;
static uint8_t m_rate = 2;      // 2 Mbit fills the channel faster
static uint8_t m_power = TX_POWER_MIN;

static const char *mode_name(uint8_t mode)
{
    switch (mode)
    {
    case JAM_SWEEP:
        return "SWEEP 2400-2483";
    case JAM_WIFI:
        return "WIFI CHANNEL";
    default:
        return "NOISE 1 CHANNEL";
    }
}

static void rows_draw(void)
{
    static const char *const items[ROW_COUNT] = {"Back", "Mode", "Channel", "Rate", "Power"};
    static char ch_buf[10];
    const char *vals[ROW_COUNT];

    uint16_t mhz = m_mode == JAM_WIFI ? jam_wifi_channel_mhz(m_wifi_chan) : m_mhz;
    gfx_fmt_int(ch_buf, (int)mhz);
    vals[ROW_BACK] = "";
    vals[ROW_MODE] = mode_name(m_mode);
    vals[ROW_CH] = ch_buf;
    vals[ROW_RATE] = m_rate == 2 ? "2M" : "1M";
    vals[ROW_PWR] = tx_power_name(m_power);

    ui_list("JAMMER", items, ROW_COUNT, m_row_sel, vals);

    gfx_text_micro(44, 2, "HOLD MID:START");
}

static void jam_enter(void)
{
    m_active = false;
    m_row_sel = 0;
}

static void jam_leave(void)
{
    if (jam_active())
    {
        jam_stop();
        ui_message("JAMMER STOPPED", 0, 600);
    }
    m_active = false;
}

static void config_tick(void)
{
    if (app_left() || app_right())
    {
        int dir = app_left() ? -1 : 1;
        switch (m_row_sel)
        {
        case ROW_MODE:
            m_mode = (uint8_t)((m_mode + JAM_MODE_COUNT + dir) % JAM_MODE_COUNT);
            break;
        case ROW_CH:
        {
            if (m_mode == JAM_WIFI)
            {
                int v = (int)m_wifi_chan + dir;
                m_wifi_chan = (uint8_t)(v < 1 ? 14 : (v > 14 ? 1 : v));
            }
            else
            {
                int v = (int)m_mhz + dir;
                m_mhz = (uint16_t)(v < 2400 ? 2400 : (v > 2483 ? 2483 : v));
            }
            break;
        }
        case ROW_RATE:
            m_rate = m_rate == 1 ? 2 : 1;
            break;
        case ROW_PWR:
            m_power = (uint8_t)((m_power + TX_POWER_COUNT + dir) % TX_POWER_COUNT);
            break;
        default:
            break;
        }
        app_redraw();
    }

    if (app_ok())
    {
        if (m_row_sel == ROW_BACK)
        {
            app_back();
            return;
        }
        m_row_sel = (uint8_t)((m_row_sel + 1) % ROW_COUNT);
        app_redraw();
    }

    // Deliberate hold, so a stray press can never start a jammer
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        app_note_input();

        jam_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = m_mode;
        cfg.mhz = m_mhz;
        cfg.wifi_chan = m_wifi_chan;
        cfg.rate = m_rate;
        cfg.power = m_power;

        if (jam_start(&cfg))
        {
            m_active = true;
            buttons_flush();
            app_redraw();
        }
        else
            ui_message("JAM FAILED", 0, 800);
        return;
    }

    if (app_take_redraw())
        rows_draw();
}

static void active_draw(uint32_t now)
{
    char buf[10];

    display_clear();
    ui_title("JAMMING");
    ui_battery();

    gfx_text_micro(2, 14, mode_name(m_mode));
    gfx_fmt_int(buf, (int)jam_current_mhz());
    gfx_text_micro(2, 24, buf);
    gfx_text_micro(2 + gfx_text_micro_width(buf) + 1, 24, "MHZ");

    gfx_fmt_int(buf, (int)jam_packets_sent());
    gfx_text_micro(2, 34, buf);
    gfx_text_micro(2 + gfx_text_micro_width(buf) + 1, 34, "BURSTS");

    gfx_fmt_int(buf, (int)(jam_remaining_ms(now) / 1000u));
    gfx_text_micro(2, 44, buf);
    gfx_text_micro(2 + gfx_text_micro_width(buf) + 1, 44, "S LEFT");

    gfx_text_micro(2, 56, "ANY BUTTON STOPS");
    display_flush();
}

static void active_tick(uint32_t now)
{
    jam_pump();
    jam_update(now);

    if (!jam_active())
    {
        ui_message("JAM DONE", "TIME LIMIT", 700);
        m_active = false;
        app_redraw();
        return;
    }

    if (app_any())
    {
        app_back();
        return;
    }

    // The countdown moves: redraw often while active
    static uint32_t last_ms;
    if (now - last_ms >= 200)
    {
        last_ms = now;
        active_draw(now);
    }
}

static void jam_tick(uint32_t now)
{
    if (m_active)
        active_tick(now);
    else
        config_tick();
}

const app_screen_t scr_jam = {
    .name = "Jammer",
    .group = APP_GROUP_TRANSMIT,
    .enter = jam_enter,
    .tick = jam_tick,
    .leave = jam_leave,
    .busy = true,
};
