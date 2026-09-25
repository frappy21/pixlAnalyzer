#include <string.h>

#include "ant_rx.h"

#ifndef ANT_RX_HOST_TEST
#include "nrf.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"
#endif

// ANT broadcast frame after the 2-byte address match [0xA4][0x08]:
// [type=0x4E][channel][d0..d7][checksum] = 11 bytes
#define ANT_BUF_LEN 11
#define ANT_SYNC    0xA4u
#define ANT_LEN     0x08u
#define ANT_TYPE_BROADCAST 0x4Eu

// The two address bytes are consumed by the radio correlator, so we
// must include them when verifying the checksum.
bool ant_frame_valid(const uint8_t *data)
{
    uint8_t xor = ANT_SYNC ^ ANT_LEN;
    for (int i = 0; i < ANT_BUF_LEN; i++)
        xor ^= data[i];
    return xor == 0;
}

// ---------------------------------------------------------------------------
// Device table
// ---------------------------------------------------------------------------

typedef struct
{
    ant_dev_t dev[ANT_MAX_DEVICES];
    uint8_t count;
    uint8_t buf[ANT_BUF_LEN];
} ant_work_t;

static ant_work_t *m_w;

bool ant_rx_init(void *mem, size_t size)
{
    if (!mem || size < sizeof(ant_work_t) || ((uintptr_t)mem & 3u))
    {
        m_w = 0;
        return false;
    }
    m_w = (ant_work_t *)mem;
    memset(m_w, 0, sizeof(*m_w));
    return true;
}

static ant_dev_t *dev_slot(uint8_t channel)
{
    for (uint8_t i = 0; i < m_w->count; i++)
    {
        if (m_w->dev[i].channel == channel)
            return &m_w->dev[i];
    }
    if (m_w->count < ANT_MAX_DEVICES)
    {
        ant_dev_t *d = &m_w->dev[m_w->count++];
        memset(d, 0, sizeof(*d));
        d->channel = channel;
        return d;
    }
    // Full: replace the oldest (lowest last_ms)
    ant_dev_t *oldest = &m_w->dev[0];
    for (uint8_t i = 1; i < m_w->count; i++)
    {
        if (m_w->dev[i].last_ms < oldest->last_ms)
            oldest = &m_w->dev[i];
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->channel = channel;
    return oldest;
}

static void decode_frame(const uint8_t *buf, uint8_t rssi, uint32_t now)
{
    if (!m_w)
        return;

    // buf[0] = type (already verified == 0x4E)
    uint8_t channel = buf[1];
    const uint8_t *data = buf + 2; // data[0..7]
    uint8_t page = data[0];

    ant_dev_t *d = dev_slot(channel);
    d->page = page;
    memcpy(d->raw, data, 8);
    if (!d->rssi || rssi < d->rssi)
        d->rssi = rssi;
    d->last_ms = now;
    if (d->frames < 0xFFFFFFFFu)
        d->frames++;

    // Profile decode: HR monitor page 0x04 is the most reliable open profile.
    // data[6] = computed heart rate in bpm; 0 means invalid.
    if (page == 0x04 && data[6] > 0 && data[6] <= 240)
    {
        d->profile = ANT_PROF_HR;
        d->hr_bpm = data[6];
    }
    // Power meter main page 0x10: data[6..7] = accumulated power (little-endian watts)
    else if ((page == 0x10 || page == 0x11) && d->profile != ANT_PROF_HR)
    {
        uint16_t w = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
        d->profile = ANT_PROF_POWER;
        d->power_w = w;
    }
}

uint8_t ant_rx_count(void)
{
    return m_w ? m_w->count : 0;
}

const ant_dev_t *ant_rx_device(uint8_t index)
{
    if (!m_w || index >= m_w->count)
        return 0;
    return &m_w->dev[index];
}

// ---------------------------------------------------------------------------
// Radio receive  (target only)
// ---------------------------------------------------------------------------

#ifndef ANT_RX_HOST_TEST

static bool wait_event(volatile uint32_t *ev, uint32_t spins)
{
    for (uint32_t i = 0; i < spins; i++)
    {
        if (*ev)
        {
            *ev = 0;
            return true;
        }
    }
    return false;
}

static void radio_ant_setup(void)
{
    radio_disable();

    // 1 Mbit GFSK — same physical layer as ANT
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_1Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // No length or S fields: static ANT_BUF_LEN bytes always received
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);
    NRF_RADIO->PCNF1 = ((uint32_t)ANT_BUF_LEN << RADIO_PCNF1_MAXLEN_Pos) |
                       ((uint32_t)ANT_BUF_LEN << RADIO_PCNF1_STATLEN_Pos) |
                       (0u << RADIO_PCNF1_BALEN_Pos) | // 1-byte base
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // Address: prefix = ANT sync (0xA4), base[31:24] = broadcast length (0x08).
    // On air (big-endian, BALEN=0): PREAMBLE(0xAA) | 0xA4 | 0x08 | payload
    // This matches every standard ANT broadcast frame.
    NRF_RADIO->PREFIX0 = ANT_SYNC;
    NRF_RADIO->BASE0 = (uint32_t)ANT_LEN << 24;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;

    // 2457 MHz = 2400 + 57
    NRF_RADIO->FREQUENCY = 57;
    NRF_RADIO->PACKETPTR = (uint32_t)m_w->buf;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
}

uint16_t ant_rx_run(uint32_t window_ms)
{
    if (!m_w)
        return 0;

    radio_hfxo_start();
    radio_ant_setup();

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY, 200000))
    {
        radio_disable();
        scanner_init();
        return 0;
    }

    uint32_t start = systime_ms();
    uint16_t decoded = 0;

    while (systime_ms() - start < window_ms)
    {
        power_watchdog_feed();

        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        if (!wait_event(&NRF_RADIO->EVENTS_END, 4000000))
            break;

        uint8_t rssi = (uint8_t)(NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk);

        if (m_w->buf[0] == ANT_TYPE_BROADCAST && ant_frame_valid(m_w->buf))
        {
            decode_frame(m_w->buf, rssi, systime_ms());
            decoded++;
        }
    }

    radio_disable();
    scanner_init();
    return decoded;
}

#else // ANT_RX_HOST_TEST

void ant_test_inject(const uint8_t *buf, uint8_t rssi, uint32_t ms)
{
    if (ant_frame_valid(buf))
        decode_frame(buf, rssi, ms);
}

#endif // ANT_RX_HOST_TEST
