#include <string.h>

#include "app_config.h"
#include "classify.h"

#ifndef CLASSIFY_HOST_TEST
#include "scanner.h"
#endif

#define PERIOD_BUCKETS 64
#define BUCKET_US 2000u // 2ms resolution up to 128ms, longer periods fold in

// Mains cycle: a microwave oven emits on one half of every mains period
#define MAINS_50HZ_US 20000u
#define MAINS_60HZ_US 16667u

static bool near(uint32_t value, uint32_t target, uint32_t tolerance_percent)
{
    uint32_t slack = (target * tolerance_percent) / 100u;
    return value + slack >= target && value <= target + slack;
}

static uint16_t median_length(const burst_t *bursts, uint16_t count)
{
    if (count == 0)
        return 0;

    // Counting sort over a coarse length histogram: bursts are short and we
    // only need a robust middle, not an exact median
    static uint16_t hist[64];
    memset(hist, 0, sizeof(hist));

    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t bucket = bursts[i].len_us / 50; // 50us buckets, up to 3.2ms
        if (bucket > 63)
            bucket = 63;
        hist[bucket]++;
    }

    uint16_t half = count / 2;
    uint16_t seen = 0;
    for (uint16_t b = 0; b < 64; b++)
    {
        seen += hist[b];
        if (seen > half)
            return (uint16_t)(b * 50 + 25);
    }
    return 0;
}

// Dominant repetition period from the gaps between burst starts
static uint32_t dominant_period(const burst_t *bursts, uint16_t count, uint16_t *spread_out)
{
    if (spread_out)
        *spread_out = 100;
    if (count < 4)
        return 0;

    static uint16_t hist[PERIOD_BUCKETS];
    memset(hist, 0, sizeof(hist));

    for (uint16_t i = 1; i < count; i++)
    {
        uint32_t gap = bursts[i].start_us - bursts[i - 1].start_us;
        uint32_t bucket = gap / BUCKET_US;
        if (bucket == 0 || bucket >= PERIOD_BUCKETS)
            continue;
        hist[bucket]++;
    }

    uint16_t best = 0;
    uint16_t best_count = 0;
    for (uint16_t b = 1; b < PERIOD_BUCKETS; b++)
    {
        if (hist[b] > best_count)
        {
            best_count = hist[b];
            best = b;
        }
    }

    // A period only counts when a decent share of the gaps agree on it
    if (best == 0 || best_count < 3 || best_count * 4 < count)
        return 0;

    // Refine: average the gaps that fall inside the winning bucket
    uint32_t sum = 0;
    uint32_t n = 0;
    uint32_t lo = best * BUCKET_US;
    uint32_t hi = lo + BUCKET_US;
    for (uint16_t i = 1; i < count; i++)
    {
        uint32_t gap = bursts[i].start_us - bursts[i - 1].start_us;
        if (gap >= lo && gap < hi)
        {
            sum += gap;
            n++;
        }
    }
    if (n == 0)
        return 0;

    uint32_t period = sum / n;
    if (spread_out)
        *spread_out = (uint16_t)(100u - (best_count * 100u) / (count - 1));
    return period;
}

void classify_features(uint16_t mhz, const burst_t *bursts, uint16_t count,
                       const park_stats_t *stats, classify_features_t *out)
{
    memset(out, 0, sizeof(*out));

    if (stats && stats->window_us)
        out->duty_ppm = (uint32_t)((uint64_t)stats->on_us * 1000000u / stats->window_us);

    out->bursts = count;
    out->median_len_us = median_length(bursts, count);
    out->period_us = dominant_period(bursts, count, &out->period_spread);
    out->peak_rssi = stats ? stats->peak_rssi : RSSI_INVALID;
    out->on_ble_channel = (mhz == 2402 || mhz == 2426 || mhz == 2480);
    out->mains_locked = out->period_us &&
                        (near(out->period_us, MAINS_50HZ_US, 8) || near(out->period_us, MAINS_60HZ_US, 8));
    out->width_mhz = classify_width(mhz);
}

uint8_t classify_decide(const classify_features_t *f, uint8_t *confidence)
{
    uint8_t conf = 40;
    uint8_t kind = VERDICT_UNKNOWN;

    if (f->bursts == 0 && f->duty_ppm < 2000)
    {
        if (confidence)
            *confidence = 90;
        return VERDICT_QUIET;
    }

    if (f->duty_ppm > 900000u)
    {
        kind = VERDICT_CONTINUOUS;
        conf = 85;
    }
    else if (f->mains_locked && f->duty_ppm > 200000u && f->width_mhz >= 6)
    {
        // Half a mains cycle of very strong wideband noise is an oven, and it
        // is the one label this device can put on the screen without hedging
        kind = VERDICT_MICROWAVE;
        conf = f->peak_rssi < 55 ? 90 : 75;
    }
    else if (f->width_mhz >= 12)
    {
        if (f->period_us && near(f->period_us, 102400u, 12) && f->duty_ppm < 200000u)
        {
            kind = VERDICT_WIFI_BEACON;
            conf = 85;
        }
        else
        {
            kind = VERDICT_WIFI;
            conf = f->duty_ppm > 50000u ? 75 : 55;
        }
    }
    else if (f->on_ble_channel && f->median_len_us >= 150 && f->median_len_us <= 2000 &&
             f->width_mhz <= 4)
    {
        kind = VERDICT_BLE_ADV;
        conf = 65; // the BLE receiver upgrades this to certainty
    }
    else if (f->median_len_us && f->median_len_us < 400 && f->width_mhz <= 3)
    {
        kind = VERDICT_NARROW_BURST;
        conf = 55;
    }
    else if (f->duty_ppm < 20000u && f->bursts >= 4 && f->width_mhz <= 4)
    {
        kind = VERDICT_HOPPER;
        conf = 45;
    }

    // Short captures never justify a strong claim
    if (f->bursts < 8 && kind != VERDICT_CONTINUOUS && kind != VERDICT_QUIET)
        conf = conf > 25 ? conf - 25 : 10;

    if (confidence)
        *confidence = conf;
    return kind;
}

void classify_run(uint16_t mhz, const burst_t *bursts, uint16_t count,
                  const park_stats_t *stats, verdict_t *out)
{
    classify_features(mhz, bursts, count, stats, &out->f);
    out->kind = classify_decide(&out->f, &out->confidence);
}

const char *classify_name(uint8_t kind)
{
    switch (kind)
    {
    case VERDICT_QUIET:
        return "QUIET";
    case VERDICT_CONTINUOUS:
        return "CONTINUOUS";
    case VERDICT_WIFI:
        return "WIFI-LIKE";
    case VERDICT_WIFI_BEACON:
        return "WIFI BEACON";
    case VERDICT_MICROWAVE:
        return "MICROWAVE";
    case VERDICT_BLE_ADV:
        return "BLE ADV";
    case VERDICT_NARROW_BURST:
        return "NARROW BURST";
    case VERDICT_HOPPER:
        return "HOPPING";
    default:
        return "UNKNOWN";
    }
}

#ifndef CLASSIFY_HOST_TEST

uint8_t classify_width(uint16_t mhz)
{
    uint8_t count = scanner_count();
    int center = -1;

    for (uint8_t i = 0; i < count; i++)
    {
        if (scanner_mhz(i) == mhz)
        {
            center = i;
            break;
        }
    }
    if (center < 0)
        return 0;

    // Walk out from the centre while channels are still busy
    uint8_t width = 1;
    for (int i = center - 1; i >= 0; i--)
    {
        if (g_scan[i].busy < 16 && g_scan[i].peak >= g_floor[i] - SIGNAL_MARGIN_DB)
            break;
        width++;
    }
    for (int i = center + 1; i < count; i++)
    {
        if (g_scan[i].busy < 16 && g_scan[i].peak >= g_floor[i] - SIGNAL_MARGIN_DB)
            break;
        width++;
    }
    return width;
}

#endif
