/**
 * ESB sniffer screens: the SNIFF main screen (channel activity by decoded
 * ShockBurst address) and the packet browser opened from it.
 *
 * Main screen: short LEFT/RIGHT step the channel lock through HOP, 2400..2483
 * (HOP listens to each channel in turn), short MID opens the packet browser
 * when anything was decoded. The browser: LEFT/RIGHT step through the ring
 * of decoded packets, MID captures the shown packet for the ESB TX screen.
 * Both keep listening.
 *
 * The sniffer state lives in the lower, main screen half of the arena, like
 * the Zigbee receiver's. The capture mailbox is outside the arena, so it
 * survives until the ESB TX screen uses it.
 */
#include <string.h>

#include "app.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"
#include "ui_sniff.h"

// One slice of listening per main loop pass, so the buttons stay responsive
#define SNIFF_SLICE_MHZ 1
#define SNIFF_DWELL_MS 30
#define SNIFF_UPDATE_MS 300

// The sniffer state lives in the lower, main screen half of the arena
#define SNIFF_ARENA_BYTES 8192
_Static_assert(sizeof(esb_sniff_work_t) <= SNIFF_ARENA_BYTES,
               "ESB sniffer state does not fit its arena share");

// Session only: channel lock (0 = hop)
static uint16_t m_lock;
static uint16_t m_hop_mhz = 2400;
static uint32_t m_redraw_ms;

extern const app_screen_t scr_sniff_pkt;

static void sniff_enter(void)
{
    esb_sniff_init(g_app_arena, SNIFF_ARENA_BYTES);
    m_hop_mhz = 2400;
    m_redraw_ms = 0;
}

// One slice of listening, shared by the list and the browser
static void listen(uint32_t now)
{
    uint16_t mhz = m_lock ? m_lock : m_hop_mhz;
    uint16_t end = mhz + SNIFF_SLICE_MHZ - 1;
    esb_sniff_run(mhz, end, SNIFF_DWELL_MS, g_settings.sniff_rate, g_settings.sniff_bits != 0);
    if (!m_lock)
    {
        m_hop_mhz = m_hop_mhz >= 2483 ? 2400 : (uint16_t)(m_hop_mhz + 1);
    }

    if (now - m_redraw_ms >= SNIFF_UPDATE_MS)
    {
        m_redraw_ms = now;
        app_redraw();
    }
}

static void sniff_tick(uint32_t now)
{
    listen(now);

    // HOP, 2400, 2401, ... 2483, HOP
    if (app_left())
    {
        m_lock = m_lock == 0 ? 2483 : (m_lock <= 2400 ? 0 : (uint16_t)(m_lock - 1));
        app_redraw();
    }
    if (app_right())
    {
        m_lock = m_lock == 0 ? 2400 : (m_lock >= 2483 ? 0 : (uint16_t)(m_lock + 1));
        app_redraw();
    }
    if (app_ok())
    {
        if (esb_sniff_pkt_count())
            app_open(&scr_sniff_pkt);
    }

    if (app_take_redraw())
        ui_sniff_list(m_lock, m_hop_mhz, esb_sniff_locks(), esb_sniff_decoded());
}

const app_screen_t scr_sniff = {
    .name = "ESB snif",
    .group = APP_GROUP_RECEIVE,
    .enter = sniff_enter,
    .tick = sniff_tick,
    .busy = true,
};

// ---------------------------------------------------------------------------
// Packet browser, opened from the list
// ---------------------------------------------------------------------------

static uint8_t m_pkt_sel;

static void pkt_enter(void)
{
    m_pkt_sel = 0;
}

static void pkt_tick(uint32_t now)
{
    listen(now);

    uint8_t count = esb_sniff_pkt_count();

    if (app_left())
    {
        m_pkt_sel = m_pkt_sel ? (uint8_t)(m_pkt_sel - 1) : (uint8_t)(count ? count - 1 : 0);
        app_redraw();
    }
    if (app_right())
    {
        m_pkt_sel = count ? (uint8_t)((m_pkt_sel + 1) % count) : 0;
        app_redraw();
    }
    if (m_pkt_sel >= count)
        m_pkt_sel = count ? (uint8_t)(count - 1) : 0;

    if (app_ok())
    {
        const esb_pkt_t *p = esb_sniff_pkt(m_pkt_sel);
        if (p)
        {
            esb_sniff_capture_set(p);
            ui_message("CAPTURED", "READY FOR ESB TX", 900);
            app_redraw();
        }
    }

    if (app_take_redraw())
    {
        const esb_pkt_t *p = esb_sniff_pkt(m_pkt_sel);
        if (!p)
        {
            app_back();
            return;
        }
        esb_pkt_t held;
        bool captured = false;
        if (esb_sniff_capture_get(&held) && held.ms == p->ms && held.mhz == p->mhz &&
            held.raw_len == p->raw_len && memcmp(held.raw, p->raw, p->raw_len) == 0)
            captured = true;
        ui_sniff_pkt(p, (uint8_t)(m_pkt_sel + 1), count, captured);
    }
}

const app_screen_t scr_sniff_pkt = {
    .name = "ESB packets",
    .group = APP_GROUP_HIDDEN,
    .enter = pkt_enter,
    .tick = pkt_tick,
    .busy = true,
};
