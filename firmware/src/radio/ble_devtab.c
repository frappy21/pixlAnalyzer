#include <string.h>

#include "ble_devtab.h"

// PDU types of the legacy advertising channel PDUs that carry AD data
#define PDU_ADV_IND 0x00
#define PDU_SCAN_RSP 0x04

static ble_dev_t *m_table;
static uint8_t m_capacity;
static uint8_t m_count;

// Spam flood detector: new addresses per 500 ms bucket over a 2 s ring
static uint8_t m_spam_ring[BLE_SPAM_BUCKETS];
static uint32_t m_spam_bucket;
static uint32_t m_spam_last_ms;
static bool m_spam_seen;
static uint16_t m_spam_family[BLE_SPAM_COUNT];
static ble_spam_state_t m_spam;

void ble_devtab_clear(void)
{
    if (m_table)
        memset(m_table, 0, (uint32_t)m_capacity * sizeof(ble_dev_t));
    m_count = 0;
    memset(m_spam_ring, 0, sizeof(m_spam_ring));
    memset(m_spam_family, 0, sizeof(m_spam_family));
    memset(&m_spam, 0, sizeof(m_spam));
    m_spam_bucket = 0;
    m_spam_seen = false;
}

void ble_devtab_attach(ble_dev_t *table, uint8_t capacity)
{
    m_table = table;
    m_capacity = table ? capacity : 0;
    ble_devtab_clear();
}

bool ble_devtab_attached(void) { return m_table != 0; }

uint8_t ble_devtab_count(void) { return m_count; }

const ble_dev_t *ble_devtab_device(uint8_t index)
{
    return index < m_count ? &m_table[index] : 0;
}

static bool addr_match(const ble_dev_t *d, const uint8_t *addr, uint8_t addr_type)
{
    return d->addr_type == addr_type && memcmp(d->addr, addr, 6) == 0;
}

const ble_dev_t *ble_devtab_find(const uint8_t *addr, uint8_t addr_type)
{
    for (uint8_t i = 0; i < m_count; i++)
    {
        if (addr_match(&m_table[i], addr, addr_type))
            return &m_table[i];
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Spam flood detector
// ---------------------------------------------------------------------------

static void spam_roll(uint32_t now_ms)
{
    uint32_t bucket = now_ms / BLE_SPAM_BUCKET_MS;
    uint32_t steps = bucket - m_spam_bucket;
    if (steps > BLE_SPAM_BUCKETS)
        steps = BLE_SPAM_BUCKETS;
    for (uint32_t s = 1; s <= steps; s++)
        m_spam_ring[(m_spam_bucket + s) % BLE_SPAM_BUCKETS] = 0;
    m_spam_bucket = bucket;

    uint16_t rate = 0;
    for (uint8_t i = 0; i < BLE_SPAM_BUCKETS; i++)
        rate = (uint16_t)(rate + m_spam_ring[i]);
    m_spam.rate = rate;
}

static void spam_count(uint8_t family, uint32_t now_ms)
{
    spam_roll(now_ms);
    m_spam.active = m_spam_seen && (now_ms - m_spam_last_ms) < BLE_SPAM_HOLD_MS;
    uint8_t *slot = &m_spam_ring[m_spam_bucket % BLE_SPAM_BUCKETS];
    if (*slot < 255)
        (*slot)++;
    m_spam.rate++;
    m_spam.total++;

    if (!m_spam.active)
        memset(m_spam_family, 0, sizeof(m_spam_family)); // count per flood
    m_spam_family[family]++;

    if (m_spam.rate >= BLE_SPAM_THRESHOLD)
    {
        m_spam.active = true;
        m_spam_seen = true;
        m_spam_last_ms = now_ms;
        if (m_spam.rate > m_spam.peak)
            m_spam.peak = m_spam.rate;

        uint8_t best = BLE_SPAM_NONE;
        for (uint8_t f = 1; f < BLE_SPAM_COUNT; f++)
        {
            if (m_spam_family[f] > m_spam_family[best])
                best = f;
        }
        m_spam.family = best;
    }
}

const ble_spam_state_t *ble_devtab_spam(uint32_t now_ms)
{
    spam_roll(now_ms);
    m_spam.active = m_spam_seen && (now_ms - m_spam_last_ms) < BLE_SPAM_HOLD_MS;
    return &m_spam;
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

// A full table recycles the entry heard longest ago, sparing trackers and
// followers while there is anything else to recycle
static ble_dev_t *recycle(void)
{
    ble_dev_t *victim = 0;
    for (int pass = 0; pass < 2 && !victim; pass++)
    {
        for (uint8_t i = 0; i < m_count; i++)
        {
            ble_dev_t *d = &m_table[i];
            if (pass == 0 && (ble_kind_is_tracker(d->kind) || (d->flags & BLE_DEV_FOLLOW)))
                continue;
            if (!victim || (int32_t)(d->last_ms - victim->last_ms) < 0)
                victim = d;
        }
    }
    return victim;
}

static ble_dev_t *add(const uint8_t *addr, uint8_t addr_type)
{
    ble_dev_t *d = (m_count < m_capacity) ? &m_table[m_count++] : recycle();
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, addr, 6);
    d->addr_type = addr_type;
    d->rssi = -127;
    d->rssi_min = 127;
    return d;
}

void ble_devtab_packet(const ble_rx_t *rx)
{
    if (!m_table || m_capacity == 0)
        return;

    uint8_t anon[6] = {0};
    const uint8_t *addr = rx->addr;
    if (!addr)
    {
        anon[0] = rx->sid;
        addr = anon;
    }

    ble_ad_summary_t sum;
    ble_ad_summarize(rx->ad, rx->ad_len, &sum);

    ble_dev_t *dev = (ble_dev_t *)ble_devtab_find(addr, rx->addr_type);
    if (!dev)
    {
        // A new random address with a spam prone pattern: count it for the
        // flood detector, and while a flood is on keep it out of the table
        // so the real devices are not pushed out
        if (sum.spam != BLE_SPAM_NONE && rx->addr_type == BLE_ADDR_RANDOM)
        {
            spam_count(sum.spam, rx->now_ms);
            if (m_spam.active && m_count >= m_capacity / 2)
            {
                m_spam.dropped++;
                return;
            }
        }
        dev = add(addr, rx->addr_type);
        dev->first_ms = rx->now_ms;
    }

    // Log: first and last seen, count, RSSI range
    dev->last_ms = rx->now_ms;
    if (dev->packets < 0xFFFF)
        dev->packets++;
    dev->pdu_type = rx->pdu_type;
    dev->rssi_last = rx->rssi;
    if (rx->rssi > dev->rssi)
        dev->rssi = rx->rssi;
    if (rx->rssi < dev->rssi_min)
        dev->rssi_min = rx->rssi;

    // Interval: primary channel adverts against receiver time. A scan
    // response answers someone else's request and an AUX packet was chased
    // on purpose, so neither says anything about the interval.
    if (rx->pdu_type != PDU_SCAN_RSP && !rx->aux)
    {
        if (dev->adv_count == 0)
            dev->listen_first = rx->listen_ms;
        dev->listen_last = rx->listen_ms;
        if (dev->adv_count < 0xFFFF)
            dev->adv_count++;
    }

    // Following: distinct minutes over a long span
    uint16_t minute = (uint16_t)(rx->now_ms / 60000u);
    if (dev->minutes == 0 || minute != dev->last_minute)
    {
        if (dev->minutes < 255)
            dev->minutes++;
        dev->last_minute = minute;
    }
    if (dev->last_ms - dev->first_ms >= BLE_FOLLOW_SPAN_MS && dev->minutes >= BLE_FOLLOW_MINUTES)
        dev->flags |= BLE_DEV_FOLLOW;

    // Content
    if (rx->pdu_type == PDU_ADV_IND)
        dev->flags |= BLE_DEV_CONN;
    if (sum.spam != BLE_SPAM_NONE)
        dev->flags |= BLE_DEV_SPAMMY;
    if (rx->ext || rx->aux)
    {
        dev->flags |= BLE_DEV_EXT;
        dev->kind = ble_kind_merge(dev->kind, BLE_KIND_EXT);
    }
    if (rx->has_auxptr)
    {
        dev->aux_chan = rx->aux_chan;
        dev->aux_phy = rx->aux_phy;
    }
    dev->kind = ble_kind_merge(dev->kind, sum.kind);
    if (sum.company)
        dev->company = sum.company;
    if (sum.name && sum.name_len)
    {
        uint8_t n = sum.name_len < BLE_NAME_LEN - 1 ? sum.name_len : BLE_NAME_LEN - 1;
        for (uint8_t i = 0; i < n; i++)
            dev->name[i] = (sum.name[i] >= 32 && sum.name[i] < 127) ? (char)sum.name[i] : '.';
        dev->name[n] = '\0';
    }

    if (rx->pdu_type == PDU_SCAN_RSP)
    {
        dev->flags |= BLE_DEV_RSP;
        dev->rsp_len = ble_ad_copy_whole(dev->rsp, BLE_AD_MAX, rx->ad, rx->ad_len);
    }
    else if (rx->aux)
    {
        dev->flags |= BLE_DEV_AUX;
        dev->adv_len = ble_ad_copy_whole(dev->adv, BLE_AD_MAX, rx->ad, rx->ad_len);
    }
    else if (rx->ad_len && !(dev->flags & BLE_DEV_AUX))
    {
        // An ADV_EXT_IND normally has no AdvData; if it does, keep it unless
        // the richer AUX data is already there
        dev->adv_len = ble_ad_copy_whole(dev->adv, BLE_AD_MAX, rx->ad, rx->ad_len);
    }
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

static bool passes(const ble_dev_t *d, uint8_t filter)
{
    switch (filter)
    {
    case BLE_FILTER_TRACKERS:
        return ble_kind_is_tracker(d->kind);
    case BLE_FILTER_FOLLOW:
        return (d->flags & BLE_DEV_FOLLOW) != 0;
    case BLE_FILTER_EXT:
        return (d->flags & BLE_DEV_EXT) != 0;
    default:
        return true;
    }
}

// True when a goes before b
static bool before(const ble_dev_t *a, const ble_dev_t *b, uint8_t sort)
{
    switch (sort)
    {
    case BLE_SORT_RECENT:
        return (int32_t)(a->last_ms - b->last_ms) > 0;
    case BLE_SORT_FIRST:
        return (int32_t)(a->first_ms - b->first_ms) < 0;
    default:
        return a->rssi > b->rssi;
    }
}

uint8_t ble_devtab_sorted(uint8_t *idx, uint8_t max, uint8_t filter, uint8_t sort)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < m_count && n < max; i++)
    {
        if (passes(&m_table[i], filter))
            idx[n++] = i;
    }

    // Insertion sort, stable, n is at most the table size
    for (uint8_t i = 1; i < n; i++)
    {
        uint8_t key = idx[i];
        int j = i - 1;
        while (j >= 0 && before(&m_table[key], &m_table[idx[j]], sort))
        {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
    return n;
}

uint16_t ble_dev_interval_ms(const ble_dev_t *dev)
{
    if (dev->adv_count < BLE_INTERVAL_MIN_ADV)
        return 0;
    uint32_t ms = (dev->listen_last - dev->listen_first) / (uint32_t)(dev->adv_count - 1);
    return ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
}

uint8_t ble_devtab_following(uint8_t mode)
{
    if (mode == BLE_FOLLOW_OFF)
        return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < m_count; i++)
    {
        const ble_dev_t *d = &m_table[i];
        if ((d->flags & BLE_DEV_FOLLOW) && (mode == BLE_FOLLOW_ALL || ble_kind_is_tracker(d->kind)))
            n++;
    }
    return n;
}
