/**
 * RC DASH: the live remote control link dashboard. Follows one ShockBurst
 * device (auto: the most active), decodes its sticks through the protocol
 * table when it is a known toy family, and shows the moving payload bytes
 * as bars when it is not.
 *
 * Known families are found by cycling the promiscuous receiver through the
 * first-address-byte candidates of the protocol table; any other nRF24
 * style link shows up through the 0x55/0xAA defaults and lands in the
 * generic view.
 *
 * Hoppers are followed the honest way: stay on the last channel, and when
 * the link goes quiet for a second and a half, sweep the band again (the
 * RQ counter on the status row).
 *
 * Short MID captures the newest packet of the tracked device for the RC
 * EMULATOR screen. The dash itself never transmits.
 */
#include <string.h>

#include "app.h"
#include "esb_sniff.h"
#include "rc_proto.h"
#include "screens.h"
#include "settings.h"
#include "ui.h"
#include "ui_rc.h"

// The sniffer state lives in the lower, main screen half of the arena, the
// one the SNIFF screen uses as well (they are never open together)
#define RC_ARENA_BYTES 8192
_Static_assert(sizeof(esb_sniff_work_t) <= RC_ARENA_BYTES,
               "RC sniffer state does not fit its arena share");

// Tracking states
enum
{
    ST_SWEEP = 0, // acquiring: sweeping the band in slices
    ST_TRACK,     // locked on a device, listening on its channel
};

#define SWEEP_SLICE 16    // MHz per tick while acquiring
#define SWEEP_DWELL_MS 2
#define TRACK_DWELL_MS 25
#define REACQUIRE_MS 1500
#define REDRAW_MS 200

static uint8_t m_state;
static uint16_t m_sweep_mhz;   // the slice start
static uint16_t m_lock_mhz;    // the channel being tracked
static uint32_t m_last_pkt_ms; // when the tracked device was last heard
static uint32_t m_last_seen_ms; // newest packet timestamp already consumed
static uint8_t m_reacquires;
static uint8_t m_pps;          // packets this second
static uint32_t m_pps_ms;

// The tracked device
static uint8_t m_addr[5];
static uint8_t m_addr_len;
static rc_proto_t m_proto;

static rc_track_t m_track; // the generic stick view
static rc_dash_view_t m_view;
static rc_sticks_t m_sticks;
static uint8_t m_flags;
static bool m_bind_phase;
static uint16_t m_rssi;

static uint32_t m_redraw_ms;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool packet_matches(const esb_pkt_t *p)
{
    if (p->f.addr_len != m_addr_len)
        return false;
    return memcmp(p->f.addr, m_addr, m_addr_len) == 0;
}

static void pick_device(void)
{
    // The most active decoded device becomes the tracked one
    uint8_t idx[ESB_SNIFF_MAX_DEV];
    uint8_t n = esb_sniff_dev_sorted(idx, ESB_SNIFF_MAX_DEV);
    if (!n)
        return;

    const esb_dev_t *d = esb_sniff_dev(idx[0]);
    if (!d || d->packets == 0)
        return;

    memcpy(m_addr, d->addr, d->addr_len);
    m_addr_len = d->addr_len;
    m_lock_mhz = d->mhz;
    m_rssi = d->rssi;
    m_proto = rc_proto_by_addr(m_addr, m_addr_len);
    m_state = ST_TRACK;
    m_last_pkt_ms = m_last_seen_ms = esb_sniff_pkt(0) ? esb_sniff_pkt(0)->ms : 0;

    rc_track_start(&m_track, rc_proto_payload_len(m_proto) ? rc_proto_payload_len(m_proto) : 10);
    memset(&m_sticks, 0, sizeof(m_sticks));
    m_pps = 0;
}

// Consumes the ring packets newer than m_last_seen_ms that belong to the
// tracked device
static void consume_ring(void)
{
    bool any = false;

    for (uint8_t i = 0; i < esb_sniff_pkt_count(); i++)
    {
        const esb_pkt_t *p = esb_sniff_pkt(i);
        if (!p || (int32_t)(p->ms - m_last_seen_ms) <= 0)
            continue;
        if (m_addr_len && !packet_matches(p))
            continue;

        m_last_seen_ms = p->ms;
        any = true;
        m_pps++;
        m_last_pkt_ms = p->ms;
        m_lock_mhz = p->mhz;
        m_rssi = p->rssi;

        // Refine the protocol guess from the actual payload: same-address
        // families (BAYANG vs JJRC, unknown vs WLToys) are told apart here.
        rc_proto_t refined = rc_proto_refine(m_proto, p->f.payload, p->f.plen);
        if (refined != m_proto)
        {
            m_proto = refined;
            rc_track_start(&m_track, rc_proto_payload_len(m_proto) ?
                           rc_proto_payload_len(m_proto) : (uint8_t)p->f.plen);
        }

        if (m_proto >= 0)
        {
            rc_sticks_t st;
            rc_bind_t bind;
            if (rc_decode(m_proto, p->f.payload, p->f.plen, &st))
            {
                m_sticks = st;
                m_flags = st.flags;
                m_bind_phase = false;
            }
            else if (rc_bind_decode(m_proto, p->f.payload, p->f.plen, &bind))
            {
                m_bind_phase = true;
            }
        }
        else
        {
            rc_track_feed(&m_track, p->f.payload, p->f.plen);
        }
    }

    if (any)
        m_state = ST_TRACK; // reacquired during a sweep
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

static void rc_enter(void)
{
    esb_sniff_init(g_app_arena, RC_ARENA_BYTES);
    m_state = ST_SWEEP;
    m_sweep_mhz = 2400;
    m_addr_len = 0;
    m_proto = RC_PROTO_UNKNOWN;
    m_reacquires = 0;
    m_pps = 0;
    m_pps_ms = 0;
    m_flags = 0;
    m_bind_phase = false;
    m_rssi = 128;
    m_redraw_ms = 0;
    m_last_seen_ms = 0;
    m_last_pkt_ms = 0;
    memset(&m_track, 0, sizeof(m_track));
    memset(&m_view, 0, sizeof(m_view));
}

static void view_update(uint32_t now)
{
    m_view.proto = m_proto;
    m_view.mhz = m_lock_mhz;
    m_view.locked = m_state == ST_TRACK && (int32_t)(now - m_last_pkt_ms) < 400;
    m_view.rssi = (uint8_t)m_rssi;
    m_view.pps = m_pps;
    m_view.reacquires = m_reacquires;
    m_view.flags = m_flags;
    m_view.sticks = m_sticks;
    m_view.bind = m_bind_phase;
    m_view.live_mask = rc_track_live(&m_track);

    for (uint8_t i = 0; i < RC_TRACK_MAX; i++)
    {
        m_view.centre[i] = rc_track_centre(&m_track, i);
        // The newest payload value: take it from the newest matching packet
        m_view.value[i] = m_view.centre[i];
    }
    if (m_addr_len == 0)
    {
        const esb_pkt_t *p = esb_sniff_pkt(0);
        if (p)
            for (uint8_t i = 0; i < RC_TRACK_MAX && i < p->f.plen; i++)
                m_view.value[i] = p->f.payload[i];
    }
    else
    {
        for (uint8_t i = 0; i < esb_sniff_pkt_count(); i++)
        {
            const esb_pkt_t *p = esb_sniff_pkt(i);
            if (p && packet_matches(p))
            {
                for (uint8_t b = 0; b < RC_TRACK_MAX && b < p->f.plen; b++)
                    m_view.value[b] = p->f.payload[b];
                break;
            }
        }
    }
}

static void rc_tick(uint32_t now)
{
    // The one second packet rate window
    if ((int32_t)(now - m_pps_ms) >= 1000)
    {
        m_pps_ms = now;
        m_view.pps = m_pps;
        m_pps = 0;
        app_redraw();
    }

    if (m_state == ST_TRACK)
    {
        bool payload_lsb = g_settings.sniff_bits != 0;
        esb_sniff_listen(m_lock_mhz, TRACK_DWELL_MS, ESB_SNIFF_PHY_1M, payload_lsb, 0x55, 0xAA);
        consume_ring();

        // The RC toy families are 1Mbit, but a generic device may be 2Mbit:
        // after a silent second on 1Mbit try the other PHY too
        if ((int32_t)(now - m_last_pkt_ms) > REACQUIRE_MS / 2)
            esb_sniff_listen(m_lock_mhz, 4, ESB_SNIFF_PHY_2M, payload_lsb, 0x55, 0xAA);

        if ((int32_t)(now - m_last_pkt_ms) > REACQUIRE_MS)
        {
            m_state = ST_SWEEP;
            m_sweep_mhz = 2400;
            m_reacquires++;
        }
    }

    if (m_state == ST_SWEEP)
    {
        uint16_t end = (uint16_t)(m_sweep_mhz + SWEEP_SLICE - 1);
        if (end > 2483)
            end = 2483;

        const uint8_t *a0;
        uint8_t n = rc_a0_candidates(&a0);
        esb_sniff_run_a0(m_sweep_mhz, end, SWEEP_DWELL_MS, ESB_SNIFF_PHY_1M,
                         g_settings.sniff_bits != 0, a0, n);
        consume_ring();

        if (!m_addr_len)
            pick_device();

        if (m_state == ST_SWEEP)
        {
            m_sweep_mhz = (uint16_t)(m_sweep_mhz + SWEEP_SLICE);
            if (m_sweep_mhz > 2483)
                m_sweep_mhz = 2400;
        }
    }

    // MID: capture the newest packet of the tracked device for the emulator
    if (app_ok() && m_addr_len)
    {
        for (uint8_t i = 0; i < esb_sniff_pkt_count(); i++)
        {
            const esb_pkt_t *p = esb_sniff_pkt(i);
            if (p && packet_matches(p))
            {
                esb_sniff_capture_set(p);
                ui_message("CAPTURED", "READY FOR RC EMULATOR", 900);
                break;
            }
        }
    }

    if (now - m_redraw_ms >= REDRAW_MS)
    {
        m_redraw_ms = now;
        app_redraw();
    }

    if (app_take_redraw())
    {
        view_update(now);
        ui_rc_dash(&m_view);
    }
}

const app_screen_t scr_rc = {
    .name = "RC dash",
    .group = APP_GROUP_RC,
    .enter = rc_enter,
    .tick = rc_tick,
    .busy = true,
};
