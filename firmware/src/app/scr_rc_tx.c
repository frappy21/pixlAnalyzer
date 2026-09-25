/**
 * RC EMULATOR: the transmit side of the RC dash. Takes a captured control
 * packet, edits its sticks through the protocol encoder (the payload's own
 * layout, checksum recomputed) and replays it as a short burst on the self
 * owned link it came from.
 *
 * Safety follows the house pattern: the lowest power by default, a one
 * second hold of MID to arm, any button stops the burst, a hard limit, and
 * every exit path goes through the leave that stops the transmitter. For
 * your own toys in your own lab.
 */
#include <string.h>

#include "app.h"
#include "buttons.h"
#include "esb_sniff.h"
#include "esb_tx.h"
#include "gfx.h"
#include "rc_proto.h"
#include "screens.h"
#include "tx_test.h"
#include "ui.h"
#include "ui_rc.h"

// How long a "loop" burst is allowed to run, seconds (the ESB engine caps
// at ESB_TX_MAX_MS itself)
#define RC_TX_LOOP_MS 10000

enum
{
    ROW_THR = 0,
    ROW_YAW,
    ROW_PIT,
    ROW_ROL,
    ROW_PWR,
    ROW_BURST,
    ROW_COUNT
};

// Packet count choices, index 0 is a ten second loop
static const uint16_t count_values[] = {0, 20, 100, 500};
#define COUNT_CHOICES 4

// Packet period of the toy families, in us (Bayang 2ms, Syma 4ms, H8 1.8ms,
// MJX 4ms): 2ms is a safe middle
#define RC_TX_GAP_US 2000

static bool m_active;
static uint8_t m_row_sel;
static rc_sticks_t m_sticks;
static uint8_t m_power = TX_POWER_MIN;
static uint8_t m_count_sel = 1; // 20 packets

// The frame being replayed, from the capture mailbox
static esb_pkt_t m_cap;
static rc_proto_t m_proto;

static bool have_capture(void)
{
    return m_cap.raw_len != 0 && m_cap.f.addr_len >= 2;
}

static void load_capture(void)
{
    memset(&m_cap, 0, sizeof(m_cap));
    if (esb_sniff_capture_get(&m_cap))
    {
        m_proto = rc_proto_by_addr(m_cap.f.addr, m_cap.f.addr_len);
        rc_sticks_t st;
        if (m_proto >= 0 && rc_decode(m_proto, m_cap.f.payload, m_cap.f.plen, &st))
            m_sticks = st; // start from what was captured
        else
            memset(&m_sticks, 0, sizeof(m_sticks));
    }
    else
    {
        m_proto = RC_PROTO_UNKNOWN;
    }
}

static void rc_tx_enter(void)
{
    m_active = false;
    m_row_sel = 0;
    load_capture();
}

static void rc_tx_leave(void)
{
    if (esb_tx_active())
    {
        esb_tx_stop();
        ui_message("TX STOPPED", 0, 600);
    }
    m_active = false;
}

static bool rc_tx_arm(void)
{
    if (!have_capture() || m_proto < 0)
    {
        ui_message("NO DECODED CAPTURE", "CAPTURE A KNOWN TOY FRAME", 1200);
        return false;
    }

    // Rebuild the payload from the edited sticks, keep everything else as
    // captured so the frame stays credible to the receiver
    esb_frame_t f = m_cap.f;
    if (!rc_build(m_proto, f.payload, f.plen, &m_sticks))
    {
        ui_message("BUILD FAILED", 0, 800);
        return false;
    }

    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len = esb_frame_build(raw, sizeof(raw), &f);
    if (!raw_len)
    {
        ui_message("BUILD FAILED", 0, 800);
        return false;
    }

    esb_tx_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.addr, f.addr, f.addr_len);
    cfg.addr_len = f.addr_len;
    cfg.raw_len = raw_len;
    memcpy(cfg.raw, raw, raw_len);
    cfg.rate = m_cap.rate ? m_cap.rate : 1;
    cfg.mhz = m_cap.mhz;
    cfg.power = m_power;
    cfg.count = count_values[m_count_sel] ? count_values[m_count_sel]
                                          : (uint16_t)(RC_TX_LOOP_MS * 1000u / RC_TX_GAP_US);
    cfg.gap_us = RC_TX_GAP_US;

    if (!esb_tx_start(&cfg))
    {
        ui_message("TX FAILED", 0, 800);
        return false;
    }
    return true;
}

static void config_tick(void)
{
    if (app_left() || app_right())
    {
        int dir = app_left() ? -1 : 1;
        if (m_row_sel == ROW_PWR)
            m_power = (uint8_t)((m_power + TX_POWER_COUNT + dir) % TX_POWER_COUNT);
        else if (m_row_sel == ROW_BURST)
            m_count_sel = (uint8_t)((m_count_sel + COUNT_CHOICES + dir) % COUNT_CHOICES);
        else
        {
            int v;
            int step = 8;
            switch (m_row_sel)
            {
            case ROW_THR:
                v = (int)m_sticks.throttle + dir * 16;
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                m_sticks.throttle = (uint8_t)v;
                break;
            case ROW_YAW:
                v = (int)m_sticks.yaw + dir * step;
                v = v < -100 ? -100 : (v > 100 ? 100 : v);
                m_sticks.yaw = (int8_t)v;
                break;
            case ROW_PIT:
                v = (int)m_sticks.pitch + dir * step;
                v = v < -100 ? -100 : (v > 100 ? 100 : v);
                m_sticks.pitch = (int8_t)v;
                break;
            default:
                v = (int)m_sticks.roll + dir * step;
                v = v < -100 ? -100 : (v > 100 ? 100 : v);
                m_sticks.roll = (int8_t)v;
                break;
            }
        }
        app_redraw();
    }

    if (app_ok())
    {
        m_row_sel = (uint8_t)((m_row_sel + 1) % ROW_COUNT);
        app_redraw();
    }

    // Deliberate hold, so a stray press can never put a packet on the air
    if (buttons_down(BTN_MID) && buttons_held_ms(BTN_MID) > 1000)
    {
        app_note_input();
        if (rc_tx_arm())
        {
            m_active = true;
            buttons_flush();
            app_redraw();
        }
        return;
    }

    if (app_take_redraw())
    {
        rc_tx_view_t view;
        memset(&view, 0, sizeof(view));
        view.proto = m_proto;
        view.sticks = m_sticks;
        view.power = m_power;
        view.count = count_values[m_count_sel];
        view.selected = m_row_sel;
        view.has_capture = have_capture();
        view.running = false;
        ui_rc_tx(&view);
    }
}

static void active_tick(uint32_t now)
{
    esb_tx_pump();
    esb_tx_update(now);

    if (!esb_tx_active())
    {
        ui_message("TX DONE", 0, 600);
        m_active = false;
        app_redraw();
        return;
    }

    // Any button stops the burst immediately
    if (app_any())
    {
        app_back();
        return;
    }

    (void)app_take_redraw();
    rc_tx_view_t view;
    memset(&view, 0, sizeof(view));
    view.proto = m_proto;
    view.sticks = m_sticks;
    view.power = m_power;
    view.count = count_values[m_count_sel];
    view.selected = m_row_sel;
    view.has_capture = have_capture();
    view.running = true;
    view.remaining_s = (uint8_t)(esb_tx_remaining_ms(now) / 1000u);
    ui_rc_tx(&view);
}

static void rc_tx_tick(uint32_t now)
{
    if (m_active)
        active_tick(now);
    else
        config_tick();
}

const app_screen_t scr_rc_tx = {
    .name = "RC emulator",
    .group = APP_GROUP_TRANSMIT,
    .enter = rc_tx_enter,
    .tick = rc_tx_tick,
    .leave = rc_tx_leave,
    .busy = true,
};
