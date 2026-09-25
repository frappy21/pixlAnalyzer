#include <string.h>

#include "zb_rx.h"

#ifndef ZB_RX_HOST_TEST
#include "nrf.h"

#include "power.h"
#include "scanner.h"
#include "systime.h"
#endif

static zb_work_t *m_w;

// ---------------------------------------------------------------------------
// Chip sequences and their MSK form
// ---------------------------------------------------------------------------

// Symbol 0 of the 2.4GHz O-QPSK PHY, chips c0..c31 in transmit order: IEEE
// 802.15.4-2006 table 73 (802.15.4-2020 table 12-1). The other 15 sequences of
// that table are derived from it by the two rules the standard builds them
// with, see chips_of().
static const char k_chips_sym0[] = "11011001110000110101001000101110";

// Chip sequence of one symbol, bit n = chip c_n.
//  - Symbols 1..7 are symbol 0 cyclically shifted right by 4 chips per step:
//    chip n of symbol k is chip (n - 4k) mod 32 of symbol 0.
//  - Symbols 8..15 are symbols 0..7 with every odd indexed chip inverted
//    (the conjugate: the Q channel negated).
// The host test checks the result against all 16 rows of the table.
static uint32_t chips_of(uint8_t sym)
{
    uint32_t c0 = 0;
    for (uint8_t n = 0; n < 32; n++)
    {
        if (k_chips_sym0[n] == '1')
            c0 |= 1u << n;
    }

    uint8_t shift = (uint8_t)(4u * (sym & 7u));
    uint32_t c = shift ? (c0 << shift) | (c0 >> (32u - shift)) : c0;
    if (sym & 8u)
        c ^= 0xAAAAAAAAu;
    return c;
}

// O-QPSK with half sine shaping carries even chips on I and odd chips on Q,
// each pulse two chips long and the two rails offset by one chip. Over chip
// interval n exactly one pulse rises (chip n) while the other falls (chip
// n-1), so the carrier phase turns by a quarter circle per chip: that is MSK
// at 2 Mbit/s, and the direction of the turn is the MSK bit. With a = 2c-1:
//
//   n even: s = a_n sin(t) + j a_{n-1} cos(t), from j a_{n-1} to a_n:
//           counter clockwise (+f) when a_n != a_{n-1}
//   n odd:  s = a_{n-1} cos(t) + j a_n sin(t), from a_{n-1} to j a_n:
//           counter clockwise (+f) when a_n == a_{n-1}
//
// so the MSK bit (1 = +f, as BLE GFSK defines a one) is
//
//   m_n = c_n XOR c_{n-1} XOR (n odd)
//
// The chip count per symbol is even, so n can be taken within the symbol.
// m_0 depends on c_{-1}, the last chip of the previous symbol.
static uint32_t msk_of(uint8_t sym, uint32_t prev_chip)
{
    uint32_t c = chips_of(sym);
    return c ^ ((c << 1) | prev_chip) ^ 0xAAAAAAAAu;
}

// The table assumes c_{-1} = 0 (the last chip of symbol 0, as inside the
// preamble); the decoder does not compare bit 0.
void zb_msk_table(uint32_t table[16])
{
    for (uint8_t s = 0; s < 16; s++)
        table[s] = msk_of(s, 0);
}

static uint8_t popcount32(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (uint8_t)((v * 0x01010101u) >> 24);
}

uint8_t zb_symbol_decode(const uint32_t table[16], uint32_t word, uint8_t *dist)
{
    uint8_t best = 0;
    uint8_t best_d = 33;
    for (uint8_t s = 0; s < 16; s++)
    {
        uint8_t d = popcount32((word ^ table[s]) & 0xFFFFFFFEu);
        if (d < best_d)
        {
            best_d = d;
            best = s;
        }
    }
    if (dist)
        *dist = best_d;
    return best;
}

// SFD 0xA7 goes out low nibble first: symbol 7 (after a preamble symbol 0,
// whose last chip is 0, as the table assumes), then symbol 0xA, whose first
// MSK bit follows the last chip of symbol 7.
uint32_t zb_sync_address(const uint32_t table[16])
{
    uint32_t sym_a = msk_of(0xA, chips_of(7) >> 31);
    return (table[7] >> 16) | (sym_a << 16);
}

uint16_t zb_crc16(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// MAC header
// ---------------------------------------------------------------------------

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t addr_len(uint8_t mode)
{
    return mode == ZB_AM_SHORT ? 2 : (mode == ZB_AM_EXT ? 8 : 0);
}

// Which PAN IDs are present. 2003/2006: the destination PAN comes with a
// destination address, the source PAN with a source address unless PAN ID
// compression says it equals the destination PAN. 2015 (table 7-2 of
// 802.15.4-2015) reuses the bit in more combinations.
static void pan_presence(zb_mac_t *m)
{
    bool dst = m->dst_mode != ZB_AM_NONE;
    bool src = m->src_mode != ZB_AM_NONE;

    if (m->version < 2)
    {
        m->has_dst_pan = dst;
        m->has_src_pan = src && !(m->pan_comp && dst);
        return;
    }

    if (!dst && !src)
    {
        m->has_dst_pan = m->pan_comp;
        m->has_src_pan = false;
    }
    else if (dst && !src)
    {
        m->has_dst_pan = !m->pan_comp;
        m->has_src_pan = false;
    }
    else if (!dst && src)
    {
        m->has_dst_pan = false;
        m->has_src_pan = !m->pan_comp;
    }
    else if (m->dst_mode == ZB_AM_EXT && m->src_mode == ZB_AM_EXT)
    {
        m->has_dst_pan = !m->pan_comp;
        m->has_src_pan = false;
    }
    else
    {
        m->has_dst_pan = true;
        m->has_src_pan = !m->pan_comp;
    }
}

// Superframe specification, GTS and pending address fields, then the beacon
// payload, whose first byte is a protocol ID. Zigbee (ID 0) follows it with
// the stack profile / protocol version byte, a capability byte and the
// extended PAN ID.
static void parse_beacon(const uint8_t *p, uint8_t end, uint8_t pos, zb_mac_t *m)
{
    if (pos + 4 > end)
        return;

    m->beacon = true;
    m->superframe = le16(p + pos);
    m->pan_coord = (m->superframe >> 14) & 1u;
    m->assoc_permit = (m->superframe >> 15) & 1u;
    pos += 2;

    uint8_t gts = p[pos++];
    uint8_t gts_count = gts & 7u;
    if (gts_count)
        pos = (uint8_t)(pos + 1 + 3 * gts_count); // directions + descriptors
    if (pos >= end)
        return;

    uint8_t pend = p[pos++];
    pos = (uint8_t)(pos + 2 * (pend & 7u) + 8 * ((pend >> 4) & 7u));
    if (pos >= end)
        return;

    m->beacon_payload = true;
    m->proto_id = p[pos];
    if (m->proto_id == 0 && pos + 3 <= end)
    {
        m->stack_profile = p[pos + 1] & 0x0Fu;
        m->proto_ver = p[pos + 1] >> 4;
        if (pos + 11 <= end)
        {
            memcpy(m->ext_pan, p + pos + 3, 8);
            m->has_ext_pan = true;
        }
    }
}

bool zb_mac_parse(const uint8_t *psdu, uint8_t got, bool full, zb_mac_t *m)
{
    memset(m, 0, sizeof(*m));
    if (got < 2)
        return false;

    uint16_t fc = le16(psdu);
    m->type = fc & 7u;
    m->security = (fc >> 3) & 1u;
    m->pending = (fc >> 4) & 1u;
    m->ack_req = (fc >> 5) & 1u;
    m->pan_comp = (fc >> 6) & 1u;
    m->dst_mode = (fc >> 10) & 3u;
    m->version = (fc >> 12) & 3u;
    m->src_mode = (fc >> 14) & 3u;

    // Types 4..7 of 802.15.4-2015 use other frame control layouts
    if (m->type > ZB_FT_CMD || m->version == 3)
        return false;
    if (m->dst_mode == 1 || m->src_mode == 1)
        return false;

    bool seq_suppressed = m->version == 2 && ((fc >> 8) & 1u);
    bool ie_present = m->version == 2 && ((fc >> 9) & 1u);

    pan_presence(m);

    uint8_t pos = 2;
    uint8_t need = (uint8_t)(pos + (seq_suppressed ? 0 : 1) + (m->has_dst_pan ? 2 : 0) +
                             addr_len(m->dst_mode) + (m->has_src_pan ? 2 : 0) +
                             addr_len(m->src_mode));
    if (need > got)
        return false;

    if (!seq_suppressed)
    {
        m->has_seq = true;
        m->seq = psdu[pos++];
    }
    if (m->has_dst_pan)
    {
        m->dst_pan = le16(psdu + pos);
        pos += 2;
    }
    memcpy(m->dst_addr, psdu + pos, addr_len(m->dst_mode));
    pos = (uint8_t)(pos + addr_len(m->dst_mode));
    if (m->has_src_pan)
    {
        m->src_pan = le16(psdu + pos);
        pos += 2;
    }
    memcpy(m->src_addr, psdu + pos, addr_len(m->src_mode));
    pos = (uint8_t)(pos + addr_len(m->src_mode));
    m->hdr_len = pos;

    // The PAN this frame belongs to
    if (m->has_src_pan && m->src_pan != 0xFFFFu)
    {
        m->has_pan = true;
        m->pan = m->src_pan;
    }
    else if (m->has_dst_pan && m->dst_pan != 0xFFFFu)
    {
        m->has_pan = true;
        m->pan = m->dst_pan;
    }

    // Beacon payload: only in the clear, only when the frame is complete
    if (m->type == ZB_FT_BEACON && full && !m->security && !ie_present && got >= 2)
        parse_beacon(psdu, (uint8_t)(got - 2), pos, m);

    return true;
}

// ---------------------------------------------------------------------------
// Receiver state
// ---------------------------------------------------------------------------

bool zb_rx_init(void *mem, size_t size)
{
    if (!mem || size < sizeof(zb_work_t) || ((uintptr_t)mem & 3u))
    {
        m_w = 0;
        return false;
    }
    m_w = (zb_work_t *)mem;
    zb_msk_table(m_w->msk);
    zb_rx_reset();
    return true;
}

void zb_rx_reset(void)
{
    if (!m_w)
        return;
    memset(m_w->chan, 0, sizeof(m_w->chan));
    memset(m_w->pan, 0, sizeof(m_w->pan));
    memset(&m_w->totals, 0, sizeof(m_w->totals));
    m_w->n_pans = 0;
}

static uint8_t air_symbol(const uint8_t *air, uint16_t k, uint8_t *dist)
{
    const uint8_t *p = air + 4u * k;
    uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[3] << 24);
    return zb_symbol_decode(m_w->msk, w, dist);
}

static bool addr_is_broadcast(const uint8_t *a, uint8_t mode)
{
    if (mode == ZB_AM_SHORT)
    {
        uint16_t s = le16(a);
        return s == 0xFFFFu || s == 0xFFFEu;
    }
    for (uint8_t i = 0; i < 8; i++)
    {
        if (a[i] != 0xFF)
            return false;
    }
    return true;
}

static void pan_add_addr(zb_pan_t *p, const uint8_t *a, uint8_t mode)
{
    if (mode == ZB_AM_NONE || addr_is_broadcast(a, mode))
        return;

    if (mode == ZB_AM_SHORT)
    {
        uint16_t s = le16(a);
        for (uint8_t i = 0; i < p->n_short; i++)
        {
            if (p->shorts[i] == s)
                return;
        }
        if (p->n_short < ZB_PAN_SHORTS)
            p->shorts[p->n_short++] = s;
        else
            p->flags |= ZB_PAN_MORE;
        return;
    }

    for (uint8_t i = 0; i < p->n_ext; i++)
    {
        if (memcmp(p->exts[i], a, 8) == 0)
            return;
    }
    if (p->n_ext < ZB_PAN_EXTS)
        memcpy(p->exts[p->n_ext++], a, 8);
    else
        p->flags |= ZB_PAN_MORE;
}

static zb_pan_t *pan_slot(uint16_t pan, uint8_t ch)
{
    for (uint8_t i = 0; i < m_w->n_pans; i++)
    {
        if (m_w->pan[i].pan == pan && m_w->pan[i].ch == ch)
            return &m_w->pan[i];
    }

    zb_pan_t *p;
    if (m_w->n_pans < ZB_MAX_PANS)
    {
        p = &m_w->pan[m_w->n_pans++];
    }
    else
    {
        // Full: the quietest PAN makes room
        p = &m_w->pan[0];
        for (uint8_t i = 1; i < m_w->n_pans; i++)
        {
            if (m_w->pan[i].frames < p->frames)
                p = &m_w->pan[i];
        }
    }
    memset(p, 0, sizeof(*p));
    p->pan = pan;
    p->ch = ch;
    return p;
}

static void account_pan(const zb_mac_t *m, uint8_t ch, uint8_t rssi, uint32_t now_ms)
{
    if (!m->has_pan)
        return;

    zb_pan_t *p = pan_slot(m->pan, ch);
    if (p->frames < 0xFFFFu)
        p->frames++;
    if (!p->rssi || (rssi && rssi < p->rssi))
        p->rssi = rssi;
    p->last_type = m->type;
    p->last_seq = m->seq;
    p->last_ms = now_ms;

    if (m->security)
    {
        p->flags |= ZB_PAN_SECURED;
        if (p->secured < 0xFFFFu)
            p->secured++;
    }

    // Addresses: the source belongs to this PAN; the destination only when
    // it is addressed within the same PAN
    bool src_here = !m->has_src_pan || m->src_pan == m->pan;
    bool dst_here = !m->has_dst_pan || m->dst_pan == m->pan;
    if (src_here)
        pan_add_addr(p, m->src_addr, m->src_mode);
    if (dst_here)
        pan_add_addr(p, m->dst_addr, m->dst_mode);

    if (m->beacon)
    {
        p->flags |= ZB_PAN_BEACON;
        if (p->beacons < 0xFFFFu)
            p->beacons++;

        p->flags &= (uint8_t)~(ZB_PAN_PJ | ZB_PAN_COORD | ZB_PAN_ZIGBEE);
        if (m->assoc_permit)
            p->flags |= ZB_PAN_PJ;
        if (m->pan_coord)
            p->flags |= ZB_PAN_COORD;
        if (m->beacon_payload)
        {
            p->proto_id = m->proto_id;
            if (m->proto_id == 0)
            {
                p->flags |= ZB_PAN_ZIGBEE;
                p->stack_profile = m->stack_profile;
            }
        }
        if (m->has_ext_pan)
        {
            memcpy(p->ext_pan, m->ext_pan, 8);
            p->flags |= ZB_PAN_EXT_PAN;
        }
    }

    // Thread hint: beacon with proto_id 3 (Thread 1.x), or PAN ID 0xFACE
    if ((m->beacon && m->beacon_payload && m->proto_id == 3) ||
        (m->has_pan && m->pan == 0xFACE))
        p->flags |= ZB_PAN_THREAD;
}

static void bump16(uint16_t *v)
{
    if (*v < 0xFFFFu)
        (*v)++;
}

void zb_rx_feed(const uint8_t *air, uint16_t len, uint8_t ch, uint8_t rssi, uint32_t now_ms,
                zb_rx_result_t *res)
{
    memset(res, 0, sizeof(*res));
    res->status = ZB_RX_NOSYNC;
    if (!m_w || ch < ZB_CH_FIRST || ch > ZB_CH_LAST)
        return;

    if (len < ZB_AIR_PHR + 8u)
    {
        m_w->totals.nosync++;
        return;
    }

    // The address match ended in the middle of the SFD; the PHY header, the
    // frame length in 7 bits, follows the rest of it. Symbols count from there.
    air += ZB_AIR_PHR;
    uint16_t nsym = (uint16_t)((len - ZB_AIR_PHR) / 4u);
    uint16_t k = 0;

    uint8_t d_lo, d_hi;
    uint8_t lo = air_symbol(air, k, &d_lo);
    uint8_t hi = air_symbol(air, k + 1u, &d_hi);
    k += 2;
    uint8_t flen = (uint8_t)((lo | (hi << 4)) & 0x7Fu);
    if (d_lo > ZB_SYNC_MAX_DIST || d_hi > ZB_SYNC_MAX_DIST || flen < 5)
    {
        // A false address match; 0..4 are reserved lengths (an ACK, the
        // shortest frame, is 5)
        m_w->totals.nosync++;
        return;
    }

    uint16_t avail = (uint16_t)((nsym - k) / 2u);
    uint8_t got = (uint8_t)(flen < avail ? flen : avail);
    uint8_t worst = 0;
    for (uint8_t i = 0; i < got; i++)
    {
        uint8_t d0, d1;
        uint8_t n0 = air_symbol(air, (uint16_t)(k + 2u * i), &d0);
        uint8_t n1 = air_symbol(air, (uint16_t)(k + 2u * i + 1u), &d1);
        m_w->psdu[i] = (uint8_t)(n0 | (n1 << 4));
        if (d0 > worst)
            worst = d0;
        if (d1 > worst)
            worst = d1;
    }

    res->len = flen;
    res->got = got;
    res->max_dist = worst;
    // 2 symbols per byte, 16us per symbol
    res->rest_us = (uint16_t)(32u * (flen - got));

    zb_chan_t *c = &m_w->chan[ch - ZB_CH_FIRST];
    zb_totals_t *t = &m_w->totals;
    bool trusted;

    if (got == flen)
    {
        trusted = zb_crc16(m_w->psdu, flen) == 0;
        res->status = trusted ? ZB_RX_GOOD : ZB_RX_BAD;
    }
    else
    {
        // Nothing to check the bytes against: trust clean symbols only
        trusted = worst <= ZB_TRUST_DIST;
        res->status = trusted ? ZB_RX_TRUNC : ZB_RX_BAD;
    }

    if (!trusted)
    {
        bump16(&c->bad);
        t->bad++;
        return;
    }

    if (res->status == ZB_RX_GOOD)
    {
        bump16(&c->good);
        t->good++;
    }
    else
    {
        bump16(&c->trunc);
        t->trunc++;
    }
    if (c->recent < 0xFF)
        c->recent++;
    if (!c->rssi || (rssi && rssi < c->rssi))
        c->rssi = rssi;

    res->parsed = zb_mac_parse(m_w->psdu, got, res->status == ZB_RX_GOOD, &res->mac);
    if (!res->parsed)
        return;

    t->type[res->mac.type & 3u]++;
    if (res->mac.security)
        t->secured++;
    account_pan(&res->mac, ch, rssi, now_ms);
}

void zb_rx_decay(void)
{
    if (!m_w)
        return;
    for (uint8_t i = 0; i < ZB_CH_COUNT; i++)
        m_w->chan[i].recent >>= 1;
}

const zb_totals_t *zb_rx_totals(void)
{
    return m_w ? &m_w->totals : 0;
}

const zb_chan_t *zb_rx_chan(uint8_t ch)
{
    if (!m_w || ch < ZB_CH_FIRST || ch > ZB_CH_LAST)
        return 0;
    return &m_w->chan[ch - ZB_CH_FIRST];
}

uint8_t zb_rx_pan_count(void)
{
    return m_w ? m_w->n_pans : 0;
}

const zb_pan_t *zb_rx_pan(uint8_t index)
{
    return (m_w && index < m_w->n_pans) ? &m_w->pan[index] : 0;
}

uint8_t zb_rx_sorted(uint8_t *idx, uint8_t max)
{
    uint8_t n = zb_rx_pan_count();
    if (n > max)
        n = max;
    for (uint8_t i = 0; i < n; i++)
        idx[i] = i;

    // Insertion sort, most frames first; ties keep table order
    for (uint8_t i = 1; i < n; i++)
    {
        uint8_t v = idx[i];
        uint8_t j = i;
        while (j && m_w->pan[idx[j - 1]].frames < m_w->pan[v].frames)
        {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------

#ifndef ZB_RX_HOST_TEST

static bool wait_event(volatile uint32_t *event, uint32_t spins)
{
    for (uint32_t i = 0; i < spins; i++)
    {
        if (*event)
        {
            *event = 0;
            return true;
        }
    }
    return false;
}

// BLE 2Mbit GFSK as a 2 Mchip/s MSK chip receiver: no whitening, no CRC, a
// fixed 255 byte capture, and a 4 byte address laid out exactly like a BLE
// access address (bit 0 first on air), set to the MSK pattern of the sync
// symbol
static void radio_setup(uint8_t ch)
{
    radio_disable();

    NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_2Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_16bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = (ZB_AIR_LEN << RADIO_PCNF1_MAXLEN_Pos) |
                       (ZB_AIR_LEN << RADIO_PCNF1_STATLEN_Pos) |
                       (3 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    uint32_t address = zb_sync_address(m_w->msk);
    NRF_RADIO->BASE0 = address << 8;
    NRF_RADIO->PREFIX0 = address >> 24;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->FREQUENCY = ZB_CH_MHZ(ch) - 2400u;
    NRF_RADIO->PACKETPTR = (uint32_t)m_w->air;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
}

void zb_rx_run(uint8_t ch, uint16_t dwell_ms)
{
    if (!m_w || ch < ZB_CH_FIRST || ch > ZB_CH_LAST)
        return;

    radio_hfxo_start();
    radio_setup(ch);

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1;
    if (wait_event(&NRF_RADIO->EVENTS_READY, 200000))
    {
        uint32_t start_us = systime_us();
        uint32_t window_us = (uint32_t)dwell_ms * 1000u;
        bool first = true;

        while ((systime_us() - start_us) < window_us)
        {
            if (!first)
            {
                NRF_RADIO->EVENTS_END = 0;
                NRF_RADIO->TASKS_START = 1;
            }
            first = false;

            bool got = false;
            while ((systime_us() - start_us) < window_us)
            {
                if (NRF_RADIO->EVENTS_END)
                {
                    NRF_RADIO->EVENTS_END = 0;
                    got = true;
                    break;
                }
            }
            if (!got)
                break;

            uint8_t rssi = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;
            zb_rx_result_t res;
            zb_rx_feed(m_w->air, ZB_AIR_LEN, ch, rssi, systime_ms(), &res);

            // The rest of a long frame is still on air, and its data could
            // hold the sync pattern: stay idle until it is over
            if (res.rest_us)
            {
                uint32_t until = systime_us() + res.rest_us;
                while ((int32_t)(systime_us() - until) < 0 &&
                       (systime_us() - start_us) < window_us)
                {
                }
            }
        }

        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);
    }

    power_watchdog_feed();

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();
}

#endif // ZB_RX_HOST_TEST
