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

// Burst timing. Single samples over the threshold are noise, not packets: the
// shortest real packet here is a Bluetooth ID packet of 68us. Two bursts closer
// than an inter frame space (BLE T_IFS is 150us) are one exchange.
#define EVENT_MIN_US 40u
#define EVENT_MERGE_US 200u

// Bluetooth BR/EDR: every packet starts on the 625us slot grid of the master
// clock, +-10us for the slave. The rest is our own detection jitter.
#define BT_SLOT_US 625u
#define BT_SLOT_TOL_US 40u
#define BT_SLOT_MAX_GAP_US 250000u // beyond this two crystals drift apart

// Crystal-tight cadence: 0.5 percent, and never under 300us of timing jitter
#define FIXED_MIN_US 20000u
#define FIXED_MAX_US 1000000u
#define FIXED_TOL_MIN_US 300u

// Frame grid of a hopping remote: FlySky AFHDS 2A 3.85ms, ExpressLRS 2..20ms,
// FrSky 9ms, Spektrum DSMX 11ms
#define FRAME_MIN_US 2000u
#define FRAME_MAX_US 25000u
#define FRAME_MIN_PCT 70u

// A silence this long is a gap in something that claims to be continuous
#define LONG_GAP_US 1000u
// Bursts shorter than this say little about the level
#define LEVEL_MIN_LEN_US 200u

// Hop spread: a channel counts only well clear of the noise floor, so a
// hundred sweeps of plain noise do not light up the band
#define SPREAD_MARGIN_DB 12

// Gap scratch, one entry per burst Identify can hold
#define MAX_EVENTS 256

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

static bool within(uint32_t value, uint32_t target, uint32_t tol)
{
    return value + tol >= target && value <= target + tol;
}

// Real packets as gaps between their starts: blips dropped, IFS pairs merged
static uint16_t event_gaps(const burst_t *bursts, uint16_t count, uint32_t *gaps,
                           uint16_t *events_out)
{
    uint16_t events = 0;
    uint16_t n = 0;
    uint32_t start = 0;
    uint32_t end = 0;

    for (uint16_t i = 0; i < count; i++)
    {
        if (bursts[i].len_us < EVENT_MIN_US)
            continue;

        uint32_t b_start = bursts[i].start_us;
        uint32_t b_end = b_start + bursts[i].len_us;

        if (events && b_start >= end && b_start - end < EVENT_MERGE_US)
        {
            end = b_end;
            continue;
        }
        if (events && n < MAX_EVENTS && b_start > start)
            gaps[n++] = b_start - start;
        start = b_start;
        end = b_end;
        events++;
    }

    *events_out = events;
    return n;
}

// Share of gaps that sit on the Bluetooth slot grid
static uint8_t slot_share(const uint32_t *gaps, uint16_t n)
{
    uint16_t seen = 0;
    uint16_t hits = 0;
    for (uint16_t i = 0; i < n; i++)
    {
        if (gaps[i] > BT_SLOT_MAX_GAP_US)
            continue;
        uint32_t r = gaps[i] % BT_SLOT_US;
        seen++;
        if (r <= BT_SLOT_TOL_US || r >= BT_SLOT_US - BT_SLOT_TOL_US)
            hits++;
    }
    return seen ? (uint8_t)((hits * 100u) / seen) : 0;
}

static uint32_t fixed_tol(uint32_t period)
{
    uint32_t tol = period / 200u;
    return tol < FIXED_TOL_MIN_US ? FIXED_TOL_MIN_US : tol;
}

// A cadence kept by a crystal: most gaps equal one value, or twice it when a
// packet was missed. Advertising jitter (BLE adds 0..10ms) does not fit.
static uint32_t fixed_period(const uint32_t *gaps, uint16_t n, uint8_t *pct_out)
{
    *pct_out = 0;
    if (n < 3)
        return 0;

    uint32_t best = 0;
    uint16_t best_hits = 0;
    for (uint16_t i = 0; i < n && i < 64; i++)
    {
        uint32_t p = gaps[i];
        if (p < FIXED_MIN_US || p > FIXED_MAX_US)
            continue;
        uint32_t tol = fixed_tol(p);
        uint16_t hits = 0;
        for (uint16_t j = 0; j < n; j++)
        {
            if (within(gaps[j], p, tol) || within(gaps[j], 2 * p, 2 * tol))
                hits++;
        }
        if (hits > best_hits || (hits == best_hits && p < best))
        {
            best = p;
            best_hits = hits;
        }
    }
    if (best_hits < 3)
        return 0;

    // Refine over every gap that agreed, a missed packet counted as two periods
    uint32_t tol = fixed_tol(best);
    uint64_t sum = 0;
    uint32_t periods = 0;
    for (uint16_t j = 0; j < n; j++)
    {
        if (within(gaps[j], best, tol))
        {
            sum += gaps[j];
            periods += 1;
        }
        else if (within(gaps[j], 2 * best, 2 * tol))
        {
            sum += gaps[j];
            periods += 2;
        }
    }
    *pct_out = (uint8_t)((best_hits * 100u) / n);
    return periods ? (uint32_t)(sum / periods) : best;
}

static uint32_t frame_tol(uint32_t frame)
{
    return 150u + (frame * 3u) / 100u;
}

// Multiple of the frame a gap sits on, 0 when it is off the grid
static uint32_t frame_multiple(uint32_t gap, uint32_t frame)
{
    uint32_t m = (gap + frame / 2) / frame;
    if (m == 0)
        return 0;
    return within(gap, m * frame, frame_tol(frame)) ? m : 0;
}

// The longest frame (2..25ms) whose multiples explain most gaps. A hopper only
// comes back to our channel every few frames, so its gaps are different
// multiples of the frame; a fixed-channel sender gives multiple 1 every time.
static uint32_t frame_period(const uint32_t *gaps, uint16_t n, classify_features_t *f)
{
    f->frame_pct = 0;
    f->frame_ratio = 0;
    f->frame_multiples = 0;
    if (n < 3)
        return 0;

    uint32_t best = 0;
    uint16_t best_hits = 0;
    for (uint16_t i = 0; i < n && i < 16; i++)
    {
        for (uint32_t k = 1; k <= 64; k++)
        {
            uint32_t p = gaps[i] / k;
            if (p < FRAME_MIN_US)
                break;
            if (p > FRAME_MAX_US || p <= best)
                continue;
            uint16_t hits = 0;
            for (uint16_t j = 0; j < n; j++)
            {
                if (frame_multiple(gaps[j], p))
                    hits++;
            }
            if (hits * 100u >= FRAME_MIN_PCT * n)
            {
                best = p;
                best_hits = hits;
            }
        }
    }
    if (best == 0)
        return 0;

    // Refine, then describe the multiples: how many kinds and the median
    static uint16_t mult_hist[65];
    memset(mult_hist, 0, sizeof(mult_hist));
    uint64_t sum = 0;
    uint32_t frames = 0;
    for (uint16_t j = 0; j < n; j++)
    {
        uint32_t m = frame_multiple(gaps[j], best);
        if (m == 0)
            continue;
        sum += gaps[j];
        frames += m;
        mult_hist[m > 64 ? 64 : m]++;
    }
    uint16_t seen = 0;
    for (uint16_t m = 1; m <= 64; m++)
    {
        if (mult_hist[m] == 0)
            continue;
        f->frame_multiples++;
        seen += mult_hist[m];
        if (f->frame_ratio == 0 && seen * 2u > best_hits)
            f->frame_ratio = (uint8_t)m;
    }
    f->frame_pct = (uint8_t)((best_hits * 100u) / n);
    return frames ? (uint32_t)(sum / frames) : best;
}

// Silences longer than LONG_GAP_US. The end of a burst clipped at 0xFFFF is
// unknown, so the silence after it is not counted.
static uint8_t count_long_gaps(const burst_t *bursts, uint16_t count)
{
    uint16_t n = 0;
    for (uint16_t i = 1; i < count; i++)
    {
        if (bursts[i - 1].len_us == 0xFFFF)
            continue;
        uint32_t end = bursts[i - 1].start_us + bursts[i - 1].len_us;
        if (bursts[i].start_us > end && bursts[i].start_us - end > LONG_GAP_US)
            n++;
    }
    return n > 255 ? 255 : (uint8_t)n;
}

// dB between the strongest and the weakest burst long enough to be measured
static uint8_t level_spread(const burst_t *bursts, uint16_t count)
{
    uint8_t lo = 255;
    uint8_t hi = 0;
    for (uint16_t i = 0; i < count; i++)
    {
        if (bursts[i].len_us < LEVEL_MIN_LEN_US)
            continue;
        if (bursts[i].peak < lo)
            lo = bursts[i].peak;
        if (bursts[i].peak > hi)
            hi = bursts[i].peak;
    }
    return hi >= lo ? (uint8_t)(hi - lo) : 255;
}

void classify_spread(const uint8_t *hits, uint8_t count, classify_features_t *f)
{
    f->spread_measured = true;
    f->spread_chans = 0;
    f->spread_runs = 0;
    f->spread_span = 0;

    int first = -1;
    int last = -1;
    bool in_run = false;
    for (uint8_t i = 0; i < count; i++)
    {
        if (hits[i] == 0)
        {
            in_run = false;
            continue;
        }
        f->spread_chans++;
        if (!in_run)
            f->spread_runs++;
        in_run = true;
        if (first < 0)
            first = i;
        last = i;
    }
    if (first >= 0)
        f->spread_span = (uint8_t)(last - first + 1);
}

void classify_features(uint16_t mhz, const burst_t *bursts, uint16_t count,
                       const park_stats_t *stats, classify_features_t *out)
{
    static uint32_t gaps[MAX_EVENTS];

    memset(out, 0, sizeof(*out));
    out->mhz = mhz;

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

    uint16_t n = event_gaps(bursts, count, gaps, &out->events);
    out->slot_pct = slot_share(gaps, n);
    out->fixed_period_us = fixed_period(gaps, n, &out->fixed_pct);
    out->frame_us = frame_period(gaps, n, out);
    out->long_gaps = count_long_gaps(bursts, count);
    out->level_spread = level_spread(bursts, count);

    uint32_t window = CLASSIFY_WINDOW_MS * 1000u;
    uint32_t windows = stats ? (stats->window_us + window / 2) / window : 0;
    out->seams = windows > 1 ? (uint8_t)(windows - 1) : 0;

    // A cadence past the 128ms histogram (ANT at 4Hz) still gets shown
    if (out->period_us == 0 && out->fixed_pct >= 75)
    {
        out->period_us = out->fixed_period_us;
        out->period_spread = (uint16_t)(100u - out->fixed_pct);
    }
}

// ANT+ device profiles run at 32768/N Hz with N = 8070 (heart rate) .. 8192
// (fitness equipment), i.e. 4.00..4.06Hz, and at half and double that rate
static bool ant_period(uint32_t us)
{
    return (us >= 240000u && us <= 252000u) || (us >= 480000u && us <= 504000u) ||
           (us >= 120000u && us <= 126000u);
}

// Gapless apart from our own blind seams, several MHz wide, level steady
// within a few dB: an FM video sender keeps its carrier up all the time
static bool is_video(const classify_features_t *f)
{
    return f->width_mhz >= 6 && f->long_gaps <= f->seams && f->level_spread <= 6;
}

static bool is_ant(const classify_features_t *f)
{
    return f->width_mhz <= 3 && f->events >= 4 && f->fixed_pct >= 75 &&
           ant_period(f->fixed_period_us) && f->median_len_us >= 60 &&
           f->median_len_us <= 1000 && f->duty_ppm < 10000u;
}

// 0 when it does not look like a hopping remote, the confidence otherwise
static uint8_t rc_fhss_confidence(const classify_features_t *f)
{
    if (f->width_mhz > 4 || f->duty_ppm >= 50000u || f->median_len_us == 0 ||
        f->median_len_us > 2500 || f->events < 4)
        return 0;

    // The hops must show up elsewhere in the band as well
    if (!f->spread_measured || f->spread_runs < 3)
        return 0;

    // Pseudo-random sequence: the gaps are different multiples of one frame,
    // and not one cadence with a missed packet here and there
    if (f->frame_us && f->frame_pct >= FRAME_MIN_PCT && f->frame_multiples >= 2 &&
        f->frame_ratio >= 2 && f->fixed_pct < 75)
        return 60;

    // Cyclic sequence: the channel comes back every N frames, exactly. On our
    // channel that is only a cadence, so the band has to show the hop set.
    if (f->fixed_pct >= 75 && f->fixed_period_us >= 40000u && f->fixed_period_us <= 500000u &&
        f->spread_runs >= 6)
        return 45;

    return 0;
}

// Slot grid on our channel, and pseudo-random hops over most of the band
static bool is_bt_classic(const classify_features_t *f)
{
    return f->width_mhz <= 4 && f->duty_ppm < 50000u && f->events >= 12 && f->slot_pct >= 60 &&
           f->median_len_us >= 60 && f->median_len_us <= 3000 && f->spread_measured &&
           f->spread_chans >= 16 && f->spread_runs >= 4 && f->spread_span >= 40;
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

    uint8_t rc_conf = 0;

    if (f->duty_ppm > 900000u)
    {
        if (is_video(f))
        {
            kind = VERDICT_VIDEO;
            conf = 70;
        }
        else
        {
            kind = VERDICT_CONTINUOUS;
            conf = 85;
        }
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
    else if (is_ant(f))
    {
        // 2457MHz is the ANT+ network frequency; elsewhere it is private ANT
        // or something else with a crystal cadence
        kind = VERDICT_ANT;
        conf = f->mhz == 2457 ? 75 : 55;
    }
    else if ((rc_conf = rc_fhss_confidence(f)) != 0)
    {
        kind = VERDICT_RC_FHSS;
        conf = rc_conf;
    }
    else if (is_bt_classic(f))
    {
        kind = VERDICT_BT_CLASSIC;
        conf = f->slot_pct >= 80 ? 70 : 60;
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

    // Short captures never justify a strong claim; a crystal cadence is
    // evidence in itself (4Hz gives only 8 bursts), and so is a gapless carrier
    if (f->bursts < 8 && kind != VERDICT_CONTINUOUS && kind != VERDICT_QUIET &&
        kind != VERDICT_VIDEO && kind != VERDICT_ANT)
        conf = conf > 25 ? conf - 25 : 10;

    if (confidence)
        *confidence = conf;
    return kind;
}

void classify_run(uint16_t mhz, const burst_t *bursts, uint16_t count,
                  const park_stats_t *stats, const uint8_t *hits, uint8_t hit_count,
                  verdict_t *out)
{
    classify_features(mhz, bursts, count, stats, &out->f);
    if (hits)
        classify_spread(hits, hit_count, &out->f);
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
    case VERDICT_BT_CLASSIC:
        return "BT CLASSIC";
    case VERDICT_ANT:
        return "ANT/ANT+";
    case VERDICT_VIDEO:
        return "ANALOG VIDEO";
    case VERDICT_RC_FHSS:
        return "RC FHSS";
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

void classify_spread_sample(uint8_t *hits)
{
    uint8_t count = scanner_count();
    for (uint8_t i = 0; i < count; i++)
    {
        if (g_scan[i].peak + SPREAD_MARGIN_DB <= g_floor[i] && hits[i] < 255)
            hits[i]++;
    }
}

#endif
