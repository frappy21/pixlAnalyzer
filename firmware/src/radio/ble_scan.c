#include <string.h>

#include "nrf.h"

#include "ble_scan.h"
#include "power.h"
#include "scanner.h"
#include "systime.h"

// Advertising channel plan: index 37/38/39 at 2402/2426/2480 MHz
static const uint8_t adv_freq[3] = {2, 26, 80};
static const uint8_t adv_index[3] = {37, 38, 39};

#define BLE_ACCESS_ADDRESS 0x8E89BED6u
#define BLE_CRC_POLY 0x0000065Bu
#define BLE_CRC_INIT 0x00555555u

// AD types we care about
#define AD_FLAGS 0x01
#define AD_UUID16_SOME 0x02
#define AD_UUID16_ALL 0x03
#define AD_NAME_SHORT 0x08
#define AD_NAME_FULL 0x09
#define AD_SERVICE_DATA16 0x16
#define AD_MANUFACTURER 0xFF

// Service UUIDs that identify tracker families
#define UUID_TILE 0xFEED
#define UUID_SMARTTAG 0xFD5A
#define UUID_EDDYSTONE 0xFEAA
#define UUID_GOOGLE_FMDN 0xFCF1

#define COMPANY_APPLE 0x004C
#define COMPANY_MICROSOFT 0x0006
#define COMPANY_SAMSUNG 0x0075

static ble_dev_t m_devices[BLE_MAX_DEVICES];
static uint8_t m_count;
static uint32_t m_packets;

// PDU buffer: S0 (header), LENGTH, then up to 37 payload bytes
static uint8_t m_pdu[64];

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

void ble_scan_reset(void)
{
    memset(m_devices, 0, sizeof(m_devices));
    m_count = 0;
    m_packets = 0;
}

void ble_scan_init(void)
{
    ble_scan_reset();
}

static void radio_configure(uint8_t chan_index, uint8_t freq)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);

    NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    // Advertising PDU header: 1 byte S0 (type/flags), 8 bit length field
    NRF_RADIO->PCNF0 = (1 << RADIO_PCNF0_S0LEN_Pos) |
                       (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);
    NRF_RADIO->PCNF1 = (37 << RADIO_PCNF1_MAXLEN_Pos) |
                       (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (3 << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // Access address 0x8E89BED6 as prefix + 3 byte base
    NRF_RADIO->BASE0 = (BLE_ACCESS_ADDRESS << 8) & 0xFFFFFF00u;
    NRF_RADIO->PREFIX0 = (BLE_ACCESS_ADDRESS >> 24) & 0xFF;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = BLE_CRC_POLY;
    NRF_RADIO->CRCINIT = BLE_CRC_INIT;

    // Whitening is seeded with the channel index, bit 6 always set
    NRF_RADIO->DATAWHITEIV = 0x40 | chan_index;
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)m_pdu;

    // Sampling RSSI at address match gives the level of the packet itself
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_READY = 0;
}

static ble_dev_t *find_or_add(const uint8_t *addr, uint8_t addr_type)
{
    for (uint8_t i = 0; i < m_count; i++)
    {
        if (m_devices[i].addr_type == addr_type && memcmp(m_devices[i].addr, addr, 6) == 0)
            return &m_devices[i];
    }

    if (m_count < BLE_MAX_DEVICES)
    {
        ble_dev_t *d = &m_devices[m_count++];
        memset(d, 0, sizeof(*d));
        memcpy(d->addr, addr, 6);
        d->addr_type = addr_type;
        d->rssi = -127;
        return d;
    }

    // Table full: recycle the weakest entry so strong nearby devices win
    ble_dev_t *weakest = &m_devices[0];
    for (uint8_t i = 1; i < m_count; i++)
    {
        if (m_devices[i].rssi < weakest->rssi)
            weakest = &m_devices[i];
    }
    memset(weakest, 0, sizeof(*weakest));
    memcpy(weakest->addr, addr, 6);
    weakest->addr_type = addr_type;
    weakest->rssi = -127;
    return weakest;
}

static void parse_ad(ble_dev_t *dev, const uint8_t *ad, int len)
{
    int pos = 0;
    while (pos + 1 < len)
    {
        int field_len = ad[pos];
        if (field_len == 0 || pos + field_len >= len + 1)
            break;

        uint8_t type = ad[pos + 1];
        const uint8_t *val = &ad[pos + 2];
        int val_len = field_len - 1;

        switch (type)
        {
        case AD_NAME_SHORT:
        case AD_NAME_FULL:
            if (dev->name[0] == '\0')
            {
                int n = val_len < BLE_NAME_LEN - 1 ? val_len : BLE_NAME_LEN - 1;
                for (int i = 0; i < n; i++)
                    dev->name[i] = (val[i] >= 32 && val[i] < 127) ? (char)val[i] : '.';
                dev->name[n] = '\0';
            }
            break;

        case AD_MANUFACTURER:
            if (val_len >= 2)
            {
                dev->company = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                if (dev->company == COMPANY_APPLE)
                {
                    // Apple's offline finding (AirTag class) uses type 0x12,
                    // iBeacon uses 0x02 with a length of 0x15
                    if (val_len >= 3 && val[2] == 0x12)
                        dev->kind = BLE_KIND_FINDMY;
                    else if (val_len >= 4 && val[2] == 0x02 && val[3] == 0x15)
                        dev->kind = BLE_KIND_IBEACON;
                    else if (dev->kind == BLE_KIND_PLAIN)
                        dev->kind = BLE_KIND_APPLE;
                }
                else if (dev->company == COMPANY_MICROSOFT && dev->kind == BLE_KIND_PLAIN)
                {
                    dev->kind = BLE_KIND_MICROSOFT;
                }
            }
            break;

        case AD_SERVICE_DATA16:
        case AD_UUID16_SOME:
        case AD_UUID16_ALL:
            if (val_len >= 2)
            {
                uint16_t uuid = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                switch (uuid)
                {
                case UUID_TILE:
                    dev->kind = BLE_KIND_TILE;
                    break;
                case UUID_SMARTTAG:
                    dev->kind = BLE_KIND_SMARTTAG;
                    break;
                case UUID_GOOGLE_FMDN:
                    dev->kind = BLE_KIND_GOOGLE_FMDN;
                    break;
                case UUID_EDDYSTONE:
                    if (dev->kind == BLE_KIND_PLAIN)
                        dev->kind = BLE_KIND_EDDYSTONE;
                    break;
                default:
                    break;
                }
            }
            break;

        default:
            break;
        }

        pos += field_len + 1;
    }
}

static void handle_packet(int8_t rssi, uint32_t now_ms)
{
    uint8_t header = m_pdu[0];
    uint8_t length = m_pdu[1];
    uint8_t pdu_type = header & 0x0F;
    uint8_t tx_add = (header >> 6) & 1;

    // Only advertising PDUs that start with an advertiser address
    if (length < 6 || length > 37)
        return;
    if (pdu_type > 0x06)
        return;

    const uint8_t *addr = &m_pdu[2];
    ble_dev_t *dev = find_or_add(addr, tx_add);

    if (dev->packets == 0)
        dev->first_ms = now_ms;
    dev->last_ms = now_ms;
    dev->packets++;
    dev->pdu_type = pdu_type;
    if (rssi > dev->rssi)
        dev->rssi = rssi;

    if (length > 6)
        parse_ad(dev, &m_pdu[8], length - 6);
}

uint16_t ble_scan_run(uint32_t window_ms)
{
    uint16_t received = 0;
    uint32_t per_channel = window_ms / 3;
    if (per_channel == 0)
        per_channel = 1;

    for (int c = 0; c < 3; c++)
    {
        radio_configure(adv_index[c], adv_freq[c]);

        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        if (!wait_event(&NRF_RADIO->EVENTS_READY, 200000))
            continue;

        uint32_t start_us = systime_us();
        uint32_t window_us = per_channel * 1000u;
        bool first = true;

        while ((systime_us() - start_us) < window_us)
        {
            // The READY_START shortcut already armed the first reception, only
            // the following ones need an explicit restart
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

            if (NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk)
            {
                int8_t rssi = -(int8_t)(NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk);
                handle_packet(rssi, systime_ms());
                received++;
                m_packets++;
            }

            power_watchdog_feed();
        }

        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        wait_event(&NRF_RADIO->EVENTS_DISABLED, 200000);
    }

    // Leave the radio the way the sweep engine expects to find it
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->PACKETPTR = 0;
    scanner_init();

    return received;
}

uint8_t ble_scan_count(void) { return m_count; }

const ble_dev_t *ble_scan_device(uint8_t index)
{
    return index < m_count ? &m_devices[index] : 0;
}

uint32_t ble_scan_packets(void) { return m_packets; }

uint8_t ble_scan_sorted(uint8_t *idx, uint8_t max)
{
    uint8_t n = m_count < max ? m_count : max;
    for (uint8_t i = 0; i < n; i++)
        idx[i] = i;

    // Insertion sort, strongest first. n is at most 40.
    for (uint8_t i = 1; i < n; i++)
    {
        uint8_t key = idx[i];
        int j = i - 1;
        while (j >= 0 && m_devices[idx[j]].rssi < m_devices[key].rssi)
        {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
    return n;
}

const char *ble_kind_name(uint8_t kind)
{
    switch (kind)
    {
    case BLE_KIND_APPLE:
        return "APPLE";
    case BLE_KIND_FINDMY:
        return "FINDMY";
    case BLE_KIND_TILE:
        return "TILE";
    case BLE_KIND_SMARTTAG:
        return "SMARTTAG";
    case BLE_KIND_GOOGLE_FMDN:
        return "GOOGLE";
    case BLE_KIND_MICROSOFT:
        return "MSFT";
    case BLE_KIND_EDDYSTONE:
        return "EDDYST";
    case BLE_KIND_IBEACON:
        return "IBEACON";
    default:
        return "";
    }
}
