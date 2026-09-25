#include <string.h>

#include "nrf.h"

#include "jam.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

// One noise burst: a full payload of random-looking bits at 1/2 Mbit.
// 255 bytes at 1 Mbit is about 2 ms on air.
#define JAM_PAYLOAD 255
#define JAM_BURST_PACKETS 8

// Sweep dwell per channel, in bursts
#define JAM_SWEEP_BURSTS 3

static jam_cfg_t m_cfg;
static bool m_active;
static uint32_t m_start_ms;
static uint32_t m_sent;
static uint16_t m_current_mhz;
static uint8_t m_burst_count;

// The noise payload buffer: refreshed from the LFSR before every burst so
// consecutive packets differ (a repeating pattern would leave gaps a
// receiver could lock onto)
static uint8_t m_payload[JAM_PAYLOAD];
static uint32_t m_lfsr;

static uint8_t next_random(void)
{
    // A 24 bit maximal LFSR, fast and good enough for noise
    uint32_t lfsr = m_lfsr;
    uint8_t out = 0;
    for (int i = 0; i < 8; i++)
    {
        uint32_t bit = ((lfsr >> 0) ^ (lfsr >> 3) ^ (lfsr >> 4) ^ (lfsr >> 5)) & 1;
        lfsr = (lfsr >> 1) | (bit << 23);
        out = (uint8_t)((out << 1) | bit);
    }
    m_lfsr = lfsr;
    return out;
}

static void fill_payload(void)
{
    for (int i = 0; i < JAM_PAYLOAD; i++)
        m_payload[i] = next_random();
}

static void wait_event(volatile uint32_t *event, uint32_t spins)
{
    for (uint32_t i = 0; i < spins; i++)
    {
        if (*event)
        {
            *event = 0;
            return;
        }
    }
}

static void radio_setup(uint16_t mhz, uint8_t rate)
{
    // Straight register programming, like tx_test.c: transmit mode, GFSK
    // 1 or 2 Mbit, no whitening, fixed length payload, no CRC, no header
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);

    NRF_RADIO->MODE = (rate == 2 ? RADIO_MODE_MODE_Nrf_2Mbit : RADIO_MODE_MODE_Nrf_1Mbit)
                      << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);

    NRF_RADIO->PCNF1 = (JAM_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos) |
                       (JAM_PAYLOAD << RADIO_PCNF1_STATLEN_Pos) |
                       (1 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // Address: the alternating preamble-ish 0xAA 0x55 pattern, so a
    // correlator anywhere on the channel sees something that looks like a
    // frame start. The payload behind it is pure noise.
    NRF_RADIO->BASE0 = 0x55000000u;
    NRF_RADIO->PREFIX0 = 0xAA;
    NRF_RADIO->TXADDRESS = 0;

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->FREQUENCY = (uint8_t)(mhz - 2400);
    NRF_RADIO->TXPOWER = tx_power_reg(m_cfg.power);
    NRF_RADIO->PACKETPTR = (uint32_t)m_payload;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

uint16_t jam_wifi_channel_mhz(uint8_t wifi_chan)
{
    if (wifi_chan == 0)
        wifi_chan = 1;
    if (wifi_chan > 14)
        wifi_chan = 14;
    if (wifi_chan == 14)
        return 2484;
    return (uint16_t)(2412 + 5 * (wifi_chan - 1));
}

bool jam_start(const jam_cfg_t *cfg)
{
    if (!cfg || cfg->mode >= JAM_MODE_COUNT || cfg->power >= TX_POWER_COUNT)
        return false;

    jam_stop();

    m_cfg = *cfg;
    if (cfg->mhz < 2400 || cfg->mhz > 2483)
        m_cfg.mhz = 2400;

    m_lfsr = (uint32_t)(NRF_FICR->DEVICEID[0] ^ systime_ms()) | 1u;
    fill_payload();

    m_active = true;
    m_sent = 0;
    m_burst_count = 0;
    m_start_ms = systime_ms();
    m_current_mhz = (cfg->mode == JAM_WIFI) ? jam_wifi_channel_mhz(cfg->wifi_chan) : cfg->mhz;

    radio_hfxo_start();
    return true;
}

void jam_stop(void)
{
    if (!m_active)
        return;

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);
    NRF_RADIO->PACKETPTR = 0;

    scanner_init(); // put the sweep engine's radio state back
    m_active = false;
}

bool jam_active(void)
{
    return m_active;
}

void jam_pump(void)
{
    if (!m_active)
        return;

    // The sweep moves one channel after every few bursts
    if (m_cfg.mode == JAM_SWEEP && m_burst_count >= JAM_SWEEP_BURSTS)
    {
        m_burst_count = 0;
        m_current_mhz = m_current_mhz >= 2483 ? 2400 : (uint16_t)(m_current_mhz + 1);
    }

    fill_payload();
    radio_setup(m_current_mhz, m_cfg.rate);

    for (int p = 0; p < JAM_BURST_PACKETS; p++)
    {
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        wait_event(&NRF_RADIO->EVENTS_READY, 200000);

        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;
        wait_event(&NRF_RADIO->EVENTS_END, 2000000);

        m_sent++;
        if (!m_active)
            return;
    }

    m_burst_count++;
    power_watchdog_feed();
}

void jam_update(uint32_t now_ms)
{
    if (!m_active)
        return;

    if ((uint32_t)(now_ms - m_start_ms) > JAM_MAX_MS)
        jam_stop();
}

uint32_t jam_remaining_ms(uint32_t now_ms)
{
    if (!m_active)
        return 0;
    uint32_t elapsed = now_ms - m_start_ms;
    return elapsed >= JAM_MAX_MS ? 0 : JAM_MAX_MS - elapsed;
}

uint32_t jam_packets_sent(void)
{
    return m_sent;
}

uint16_t jam_current_mhz(void)
{
    return m_current_mhz;
}
