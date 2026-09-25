#include <string.h>

#include "esb_sniff.h"

#ifndef ESB_SNIFF_HOST_TEST
#include "nrf.h"

#include "power.h"
#include "scanner.h"
#include "systime.h"
#endif

static esb_sniff_work_t *m_w;

// The replay mailbox, deliberately outside the arena: it survives screen and
// carousel switches, until the next capture overwrites it
static esb_pkt_t m_capture;
static bool m_capture_valid;

// ---------------------------------------------------------------------------
// Bookkeeping
// ---------------------------------------------------------------------------

bool esb_sniff_init(void *mem, size_t size)
{
    if (size < sizeof(esb_sniff_work_t))
        return false;
    m_w = (esb_sniff_work_t *)mem;
    esb_sniff_reset();
    return true;
}

void esb_sniff_reset(void)
{
    if (m_w)
        memset(m_w, 0, sizeof(*m_w));
}

// A lock with a constant pattern is noise triggering on the correlator, the
// same filter esb_scan.c uses. The CRC does the real filtering.
static bool capture_plausible(const uint8_t *cap, uint8_t cap_len)
{
    if (cap_len < 4)
        return false;
    uint8_t first = cap[0];
    uint8_t same = 0;
    for (uint8_t i = 1; i < cap_len; i++)
    {
        if (cap[i] == first)
            same++;
    }
    return same < cap_len - 2;
}

void esb_sniff_feed(const uint8_t *cap, uint8_t cap_len, uint8_t first_addr, uint8_t rate,
                    uint16_t mhz, uint8_t rssi, uint32_t now_ms, bool payload_lsb)
{
    if (!m_w || !capture_plausible(cap, cap_len))
        return;

    m_w->locks++;

    esb_pkt_t *slot = &m_w->pkt[m_w->ring_head];
    if (!esb_frame_decode(cap, cap_len, first_addr, payload_lsb, &slot->f))
        return;

    m_w->decoded++;

    // Keep the raw capture for the replay path: bit exact, whatever the
    // decode made of it
    slot->raw_len = cap_len > ESB_CAPTURE_MAX ? ESB_CAPTURE_MAX : cap_len;
    memcpy(slot->raw, cap, slot->raw_len);
    slot->rssi = rssi;
    slot->rate = rate;
    slot->mhz = mhz;
    slot->ms = now_ms;

    m_w->ring_head = (uint8_t)((m_w->ring_head + 1) % ESB_SNIFF_RING);
    if (m_w->ring_count < ESB_SNIFF_RING)
        m_w->ring_count++;

    // Address book: same address and length is the same device, whatever the
    // rate the sweep found it with
    uint8_t weakest = 0;
    for (uint8_t i = 0; i < m_w->n_dev; i++)
    {
        esb_dev_t *d = &m_w->dev[i];
        if (d->addr_len == slot->f.addr_len &&
            memcmp(d->addr, slot->f.addr, slot->f.addr_len) == 0)
        {
            d->packets++;
            d->rate = rate;
            d->mhz = mhz;
            d->last_ms = now_ms;
            if (rssi < d->rssi)
                d->rssi = rssi;
            return;
        }
        if (d->packets < m_w->dev[weakest].packets)
            weakest = i;
    }

    if (m_w->n_dev < ESB_SNIFF_MAX_DEV)
    {
        esb_dev_t *d = &m_w->dev[m_w->n_dev++];
        memset(d, 0, sizeof(*d));
        memcpy(d->addr, slot->f.addr, slot->f.addr_len);
        d->addr_len = slot->f.addr_len;
        d->packets = 1;
        d->rate = rate;
        d->mhz = mhz;
        d->rssi = rssi ? rssi : 100;
        d->last_ms = now_ms;
        return;
    }

    // Replace the quietest entry so the list keeps the busiest ones
    esb_dev_t *d = &m_w->dev[weakest];
    if (d->packets < 4)
    {
        memcpy(d->addr, slot->f.addr, slot->f.addr_len);
        d->addr_len = slot->f.addr_len;
        d->packets = 1;
        d->rate = rate;
        d->mhz = mhz;
        d->rssi = rssi ? rssi : 100;
        d->last_ms = now_ms;
    }
}

uint8_t esb_sniff_dev_count(void) { return m_w ? m_w->n_dev : 0; }

const esb_dev_t *esb_sniff_dev(uint8_t index)
{
    return (m_w && index < m_w->n_dev) ? &m_w->dev[index] : 0;
}

uint8_t esb_sniff_dev_sorted(uint8_t *idx, uint8_t max)
{
    if (!m_w)
        return 0;

    uint8_t n = m_w->n_dev < max ? m_w->n_dev : max;
    for (uint8_t i = 0; i < n; i++)
        idx[i] = i;
    for (uint8_t i = 1; i < n; i++)
    {
        uint8_t v = idx[i];
        uint8_t j = i;
        while (j > 0 && m_w->dev[idx[j - 1]].packets < m_w->dev[v].packets)
        {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    return n;
}

const esb_pkt_t *esb_sniff_pkt(uint8_t index)
{
    if (!m_w || index >= m_w->ring_count)
        return 0;
    // ring_head is the next write, so the newest entry is one before it
    uint8_t newest = (uint8_t)((m_w->ring_head + ESB_SNIFF_RING - 1) % ESB_SNIFF_RING);
    uint8_t slot = (uint8_t)((newest + ESB_SNIFF_RING - index) % ESB_SNIFF_RING);
    return &m_w->pkt[slot];
}

uint8_t esb_sniff_pkt_count(void) { return m_w ? m_w->ring_count : 0; }

bool esb_sniff_capture_get(esb_pkt_t *out)
{
    if (!m_capture_valid)
        return false;
    if (out)
        *out = m_capture;
    return true;
}

void esb_sniff_capture_set(const esb_pkt_t *p)
{
    if (!p)
        return;
    m_capture = *p;
    m_capture_valid = true;
}

void esb_sniff_capture_clear(void) { m_capture_valid = false; }

uint32_t esb_sniff_locks(void) { return m_w ? m_w->locks : 0; }
uint32_t esb_sniff_decoded(void) { return m_w ? m_w->decoded : 0; }

// ---------------------------------------------------------------------------
// Receiver, target only
// ---------------------------------------------------------------------------

#ifndef ESB_SNIFF_HOST_TEST

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

// Both polarities of the alternating pattern, as logical RX addresses 0
// (0xAA55, preamble 0xAA + first address byte 0x55) and 1 (0x55AA). The 40
// raw bytes that follow the match are handed to the software decoder.
static void radio_promiscuous(uint8_t freq, uint8_t rate)
{
    radio_disable();

    NRF_RADIO->MODE = (rate == 2 ? RADIO_MODE_MODE_Nrf_2Mbit : RADIO_MODE_MODE_Nrf_1Mbit)
                      << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // No length field, fixed payload: we are capturing, not receiving
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO->PCNF1 = (ESB_CAPTURE_MAX << RADIO_PCNF1_MAXLEN_Pos) |
                       (ESB_CAPTURE_MAX << RADIO_PCNF1_STATLEN_Pos) |
                       (2 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = 0x55000000u;
    NRF_RADIO->BASE1 = 0xAA000000u;
    NRF_RADIO->PREFIX0 = 0x55 << 8 | 0xAA;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 0x03; // both polarities

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)m_w->pkt[m_w->ring_head].raw;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
}

void esb_sniff_run(uint16_t start_mhz, uint16_t end_mhz, uint16_t dwell_ms, uint8_t rate_mode,
                   bool payload_lsb)
{
    if (!m_w || end_mhz < start_mhz)
        return;

    radio_hfxo_start();

    for (uint16_t mhz = start_mhz; mhz <= end_mhz; mhz++)
    {
        if (mhz < 2400 || mhz > 2500)
            continue;

        for (int r = 0; r < 2; r++)
        {
            uint8_t rate;
            if (rate_mode == ESB_SNIFF_PHY_1M)
            {
                if (r)
                    continue;
                rate = 1;
            }
            else if (rate_mode == ESB_SNIFF_PHY_2M)
            {
                if (r)
                    continue;
                rate = 2;
            }
            else // auto: 2Mbit gets the longer look, like esb_scan.c
            {
                rate = r ? 1 : 2;
            }

            radio_promiscuous((uint8_t)(mhz - 2400), rate);

            NRF_RADIO->EVENTS_READY = 0;
            NRF_RADIO->TASKS_RXEN = 1;
            if (!wait_event(&NRF_RADIO->EVENTS_READY, 200000))
                continue;

            uint32_t window_us = (uint32_t)dwell_ms * 1000u / (rate_mode == ESB_SNIFF_PHY_AUTO ? 3 : 1);
            if (window_us == 0)
                window_us = 1000;
            uint32_t start_us = systime_us();
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

                // Which polarity matched decides the first address byte the
                // match consumed
                uint8_t match = NRF_RADIO->RXMATCH & 7;
                uint8_t first_addr = match ? 0xAA : 0x55;
                uint8_t rssi = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;

                // The buffer was m_w->pkt[ring_head].raw; feed it without
                // disturbing the raw the ring write will do
                esb_sniff_feed((const uint8_t *)m_w->pkt[m_w->ring_head].raw, ESB_CAPTURE_MAX,
                               first_addr, rate, mhz, rssi, systime_ms(), payload_lsb);

                // The capture buffer is also the next ring slot: keep it
                NRF_RADIO->PACKETPTR = (uint32_t)m_w->pkt[m_w->ring_head].raw;
            }

            NRF_RADIO->EVENTS_DISABLED = 0;
            NRF_RADIO->TASKS_DISABLE = 1;
            wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);

            power_watchdog_feed();
        }
    }

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();
}

#endif // ESB_SNIFF_HOST_TEST
