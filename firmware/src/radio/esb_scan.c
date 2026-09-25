#include <string.h>

#include "nrf.h"

#include "esb_scan.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

static esb_hit_t m_hits[ESB_MAX_HITS];
static uint8_t m_count;
static uint32_t m_total;
static uint8_t m_buf[40];

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

void esb_scan_reset(void)
{
    memset(m_hits, 0, sizeof(m_hits));
    m_count = 0;
    m_total = 0;
}

void esb_scan_init(void) { esb_scan_reset(); }

static void radio_promiscuous(uint8_t freq, uint8_t rate)
{
    radio_disable();

    NRF_RADIO->MODE = (rate == 2 ? RADIO_MODE_MODE_Nrf_2Mbit : RADIO_MODE_MODE_Nrf_1Mbit)
                      << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // No length field, fixed payload, no whitening: we are not decoding, we are
    // counting how often the correlator locks onto a ShockBurst preamble.
    // BALEN=1: the match is the preamble polarity byte plus the first
    // address byte, exactly the two byte promiscuous pattern.
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) |
                       (32 << RADIO_PCNF1_STATLEN_Pos) |
                       (1 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // Address 0xAA55: the alternating pattern every ShockBurst preamble carries
    NRF_RADIO->BASE0 = 0x55000000u;
    NRF_RADIO->PREFIX0 = 0xAA;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)m_buf;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
}

// A lock is only interesting if the payload is not a constant pattern, which is
// what noise triggering on the correlator looks like.
static bool payload_plausible(void)
{
    uint8_t first = m_buf[0];
    int different = 0;
    for (int i = 1; i < 16; i++)
    {
        if (m_buf[i] != first)
            different++;
    }
    return different >= 3;
}

static void record(uint16_t mhz, uint16_t packets, uint8_t rssi, uint8_t rate)
{
    for (uint8_t i = 0; i < m_count; i++)
    {
        if (m_hits[i].mhz == mhz)
        {
            m_hits[i].packets += packets;
            if (rssi < m_hits[i].peak_rssi)
                m_hits[i].peak_rssi = rssi;
            return;
        }
    }

    if (m_count < ESB_MAX_HITS)
    {
        m_hits[m_count].mhz = mhz;
        m_hits[m_count].packets = packets;
        m_hits[m_count].peak_rssi = rssi;
        m_hits[m_count].rate = rate;
        m_count++;
        return;
    }

    // Replace the quietest channel so the list keeps the busiest ones
    uint8_t weakest = 0;
    for (uint8_t i = 1; i < m_count; i++)
    {
        if (m_hits[i].packets < m_hits[weakest].packets)
            weakest = i;
    }
    if (packets > m_hits[weakest].packets)
    {
        m_hits[weakest].mhz = mhz;
        m_hits[weakest].packets = packets;
        m_hits[weakest].peak_rssi = rssi;
        m_hits[weakest].rate = rate;
    }
}

uint8_t esb_scan_run(uint16_t start_mhz, uint16_t end_mhz, uint16_t dwell_ms)
{
    if (end_mhz < start_mhz)
        return m_count;

    radio_hfxo_start();

    for (uint16_t mhz = start_mhz; mhz <= end_mhz; mhz++)
    {
        if (mhz < 2400 || mhz > 2500)
            continue;

        // Mice and keyboards are 2Mbit far more often than 1Mbit, so give the
        // faster rate the longer look
        for (uint8_t rate = 2; rate >= 1; rate--)
        {
            radio_promiscuous((uint8_t)(mhz - 2400), rate);

            NRF_RADIO->EVENTS_READY = 0;
            NRF_RADIO->TASKS_RXEN = 1;
            if (!wait_event(&NRF_RADIO->EVENTS_READY, 200000))
                continue;

            uint32_t start_us = systime_us();
            uint32_t window_us = (uint32_t)dwell_ms * 1000u / (rate == 2 ? 2 : 4);
            uint16_t packets = 0;
            uint8_t best_rssi = 127;
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

                if (payload_plausible())
                {
                    uint8_t rssi = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;
                    if (rssi < best_rssi)
                        best_rssi = rssi;
                    packets++;
                    m_total++;
                }
            }

            NRF_RADIO->EVENTS_DISABLED = 0;
            NRF_RADIO->TASKS_DISABLE = 1;
            wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);

            if (packets)
                record(mhz, packets, best_rssi, rate);

            power_watchdog_feed();
        }
    }

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();
    return m_count;
}

uint8_t esb_scan_count(void) { return m_count; }

const esb_hit_t *esb_scan_hit(uint8_t index)
{
    return index < m_count ? &m_hits[index] : 0;
}

uint32_t esb_scan_total(void) { return m_total; }
