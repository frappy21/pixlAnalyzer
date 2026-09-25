/**
 * 802.15.4 receiver (Zigbee, Thread): channel activity, the PANs heard and a
 * detail card per PAN. Receive only.
 *
 * Main screen: short LEFT/RIGHT step the channel through HOP, 11..26 (HOP
 * listens to each channel in turn), short MID opens the busiest PAN's card.
 * On the card: LEFT/RIGHT previous/next PAN, MID next page of addresses,
 * long LEFT back. Both keep listening.
 */
#include "app.h"
#include "screens.h"
#include "ui_zigbee.h"
#include "zb_rx.h"

// One slice of listening per main loop pass, so the buttons stay responsive.
// Hopping, a full pass over the 16 channels takes about half a second.
#define ZB_SLICE_MS 30
#define ZB_REDRAW_MS 300
#define ZB_DECAY_MS 1000

// The receiver state lives in the lower, main screen half of the arena
#define ZB_ARENA_BYTES 8192
_Static_assert(sizeof(zb_work_t) <= ZB_ARENA_BYTES, "802.15.4 state does not fit its arena share");

// Session only: channel lock (0 = hop) and the PAN whose card is open
static uint8_t m_lock;
static uint8_t m_hop_ch = ZB_CH_FIRST;
static uint16_t m_sel_pan;
static uint8_t m_sel_ch;
static uint8_t m_page;
static uint32_t m_redraw_ms;
static uint32_t m_decay_ms;

extern const app_screen_t scr_zigbee_pan;

// Listens for one slice and keeps the periodic redraw and decay going.
// Shared by the overview and the card, which both keep the receiver running.
static void listen(uint32_t now)
{
    uint8_t ch = m_lock ? m_lock : m_hop_ch;
    zb_rx_run(ch, ZB_SLICE_MS);
    if (!m_lock)
        m_hop_ch = (m_hop_ch >= ZB_CH_LAST) ? ZB_CH_FIRST : (uint8_t)(m_hop_ch + 1);

    if (now - m_decay_ms >= ZB_DECAY_MS)
    {
        m_decay_ms = now;
        zb_rx_decay();
    }
    if (now - m_redraw_ms >= ZB_REDRAW_MS)
    {
        m_redraw_ms = now;
        app_redraw();
    }
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------

static void zb_enter(void)
{
    zb_rx_init(g_app_arena, ZB_ARENA_BYTES);
    m_hop_ch = ZB_CH_FIRST;
}

static void zb_tick(uint32_t now)
{
    listen(now);

    // HOP, 11, 12, ... 26, HOP
    if (app_left())
    {
        m_lock = (m_lock == 0) ? ZB_CH_LAST : (m_lock == ZB_CH_FIRST ? 0 : (uint8_t)(m_lock - 1));
        app_redraw();
    }
    if (app_right())
    {
        m_lock = (m_lock == 0) ? ZB_CH_FIRST : (m_lock == ZB_CH_LAST ? 0 : (uint8_t)(m_lock + 1));
        app_redraw();
    }
    if (app_ok())
    {
        uint8_t idx[ZB_MAX_PANS];
        if (zb_rx_sorted(idx, ZB_MAX_PANS))
        {
            const zb_pan_t *p = zb_rx_pan(idx[0]);
            m_sel_pan = p->pan;
            m_sel_ch = p->ch;
            app_open(&scr_zigbee_pan);
            return;
        }
    }

    if (app_take_redraw())
        ui_zigbee_main(m_lock);
}

const app_screen_t scr_zigbee = {
    .name = "Zigbee",
    .group = APP_GROUP_RECEIVE,
    .enter = zb_enter,
    .tick = zb_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// One PAN, opened from the overview
// ---------------------------------------------------------------------------

static void pan_select(const zb_pan_t *p)
{
    m_sel_pan = p->pan;
    m_sel_ch = p->ch;
    m_page = 0;
    app_redraw();
}

static void pan_enter(void)
{
    m_page = 0;
}

static void pan_tick(uint32_t now)
{
    listen(now);

    // The list is re-sorted as counts change: follow the PAN, not the row
    uint8_t idx[ZB_MAX_PANS];
    uint8_t n = zb_rx_sorted(idx, ZB_MAX_PANS);
    if (n == 0)
    {
        app_back();
        return;
    }
    uint8_t pos = 0;
    bool found = false;
    for (uint8_t i = 0; i < n; i++)
    {
        const zb_pan_t *p = zb_rx_pan(idx[i]);
        if (p->pan == m_sel_pan && p->ch == m_sel_ch)
        {
            pos = i;
            found = true;
            break;
        }
    }
    if (!found)
        pan_select(zb_rx_pan(idx[0])); // evicted from the table

    if (app_left())
    {
        pos = pos ? (uint8_t)(pos - 1) : (uint8_t)(n - 1);
        pan_select(zb_rx_pan(idx[pos]));
    }
    if (app_right())
    {
        pos = (uint8_t)((pos + 1) % n);
        pan_select(zb_rx_pan(idx[pos]));
    }
    if (app_ok())
    {
        m_page++;
        app_redraw();
    }

    if (app_take_redraw())
    {
        uint8_t pages = ui_zigbee_pan(zb_rx_pan(idx[pos]), pos, n, m_page);
        m_page %= pages;
    }
}

const app_screen_t scr_zigbee_pan = {
    .name = "Zigbee PAN",
    .group = APP_GROUP_HIDDEN,
    .enter = pan_enter,
    .tick = pan_tick,
    .busy = true,
};
