#include <string.h>

#include "nrf.h"

#include "dwell.h"
#include "power.h"
#include "scanner.h"
#include "sweep_order.h"
#include "systime.h"

chan_result_t g_scan[SCAN_MAX_CHANNELS];
uint8_t g_floor[SCAN_MAX_CHANNELS];

typedef struct
{
    uint8_t map;  // RADIO_FREQUENCY_MAP_Default or _Low
    uint8_t freq; // RADIO->FREQUENCY value
} chan_tune_t;

static chan_tune_t m_tune[SCAN_MAX_CHANNELS];
static uint16_t m_mhz[SCAN_MAX_CHANNELS];
static uint8_t m_count;
static uint8_t m_dwell = SCAN_DWELL_SAMPLES_DEFAULT;
static bool m_shuffle = true;
static uint16_t m_lcg = 0xACE1; // visit order randomiser, seeded deterministically

// Adaptive dwell (dwell.h): a channel counts as active for this many sweeps
// after a visit that saw a sample above the busy threshold
#define ACTIVITY_HOLD_SWEEPS 32
static bool m_adaptive = true;
static uint8_t m_activity[SCAN_MAX_CHANNELS]; // sweeps left in the active state

// Park mode feeds the watchdog this often, far inside its 8s timeout
#define PARK_WDT_FEED_US 100000u

// Bounded waits: a radio that never raises its event must not hang the UI
static bool wait_event(volatile uint32_t *event)
{
    for (uint32_t guard = 0; guard < 200000; guard++)
    {
        if (*event)
        {
            *event = 0;
            return true;
        }
    }
    return false;
}

void radio_disable(void)
{
    if ((NRF_RADIO->STATE & RADIO_STATE_STATE_Msk) ==
        (RADIO_STATE_STATE_Disabled << RADIO_STATE_STATE_Pos))
        return;

    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED);
}

void scanner_init(void)
{
    radio_disable();

    NRF_RADIO->POWER = 1;
    NRF_RADIO->MODE = (RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos);

    // Fast ramp-up: 40us instead of 140us per channel visit. This is the single
    // change that makes dwelling affordable.
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    for (int i = 0; i < SCAN_MAX_CHANNELS; i++)
    {
        g_floor[i] = NOISE_FLOOR_INIT;
        g_scan[i].peak = RSSI_INVALID;
        g_scan[i].weak = RSSI_INVALID;
        g_scan[i].busy = 0;
    }

    memset(m_activity, 0, sizeof(m_activity));

    if (m_count == 0)
        scanner_set_span(2400, 2483);
}

void scanner_stop(void)
{
    radio_disable();
}

void radio_hfxo_start(void)
{
    if ((NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_SRC_Msk | CLOCK_HFCLKSTAT_STATE_Msk)) ==
        (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos |
         CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos))
        return;

    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    for (uint32_t guard = 0; guard < 1000000; guard++)
    {
        if (NRF_CLOCK->EVENTS_HFCLKSTARTED)
            break;
    }
}

// Turns an absolute frequency into a tuning pair. The radio reaches 2400..2500
// with the default channel map and 2360..2460 with the low map.
static bool tune_for(uint16_t mhz, chan_tune_t *out)
{
    if (mhz >= FREQ_BASE_DEFAULT_MHZ && mhz <= FREQ_BASE_DEFAULT_MHZ + FREQ_CHANNEL_MAX)
    {
        out->map = RADIO_FREQUENCY_MAP_Default;
        out->freq = (uint8_t)(mhz - FREQ_BASE_DEFAULT_MHZ);
        return true;
    }
    if (mhz >= FREQ_BASE_LOW_MHZ && mhz < FREQ_BASE_DEFAULT_MHZ)
    {
        out->map = RADIO_FREQUENCY_MAP_Low;
        out->freq = (uint8_t)(mhz - FREQ_BASE_LOW_MHZ);
        return true;
    }
    return false;
}

void scanner_set_span(uint16_t start_mhz, uint16_t end_mhz)
{
    if (start_mhz < FREQ_BASE_LOW_MHZ)
        start_mhz = FREQ_BASE_LOW_MHZ;
    if (end_mhz > FREQ_BASE_DEFAULT_MHZ + FREQ_CHANNEL_MAX)
        end_mhz = FREQ_BASE_DEFAULT_MHZ + FREQ_CHANNEL_MAX;
    if (end_mhz < start_mhz)
        end_mhz = start_mhz;

    memset(m_activity, 0, sizeof(m_activity));

    uint16_t step = 1;
    uint16_t span = end_mhz - start_mhz + 1;
    if (span > SCAN_MAX_CHANNELS)
        step = (span + SCAN_MAX_CHANNELS - 1) / SCAN_MAX_CHANNELS;

    m_count = 0;
    for (uint16_t f = start_mhz; f <= end_mhz && m_count < SCAN_MAX_CHANNELS; f += step)
    {
        chan_tune_t t;
        if (!tune_for(f, &t))
            continue;
        m_tune[m_count] = t;
        m_mhz[m_count] = f;
        m_count++;
    }
}

void scanner_set_channels(const uint16_t *mhz, uint8_t count)
{
    memset(m_activity, 0, sizeof(m_activity));
    m_count = 0;
    for (uint8_t i = 0; i < count && m_count < SCAN_MAX_CHANNELS; i++)
    {
        chan_tune_t t;
        if (!tune_for(mhz[i], &t))
            continue;
        m_tune[m_count] = t;
        m_mhz[m_count] = mhz[i];
        m_count++;
    }
}

void scanner_set_dwell(uint8_t samples)
{
    if (samples < 1)
        samples = 1;
    if (samples > SCAN_DWELL_SAMPLES_MAX)
        samples = SCAN_DWELL_SAMPLES_MAX;
    m_dwell = samples;
}

void scanner_set_shuffle(bool on) { m_shuffle = on; }

void scanner_set_adaptive(bool on)
{
    m_adaptive = on;
    memset(m_activity, 0, sizeof(m_activity));
}

bool scanner_adaptive(void) { return m_adaptive; }

uint8_t scanner_count(void) { return m_count; }

uint16_t scanner_mhz(uint8_t index)
{
    return index < m_count ? m_mhz[index] : 0;
}

uint16_t scanner_span_start(void) { return m_count ? m_mhz[0] : 0; }
uint16_t scanner_span_end(void) { return m_count ? m_mhz[m_count - 1] : 0; }

static void radio_tune(const chan_tune_t *t)
{
    NRF_RADIO->FREQUENCY = ((uint32_t)t->map << RADIO_FREQUENCY_MAP_Pos) | t->freq;
}

// Visits one channel for the given number of samples and fills its result slot
static void visit(uint8_t idx, uint8_t dwell)
{
    radio_tune(&m_tune[idx]);

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
    {
        g_scan[idx].peak = RSSI_INVALID;
        g_scan[idx].busy = 0;
        return;
    }

    uint8_t strongest = RSSI_INVALID;
    uint8_t weakest = 0;
    uint8_t busy_thr = (g_floor[idx] > SIGNAL_MARGIN_DB) ? g_floor[idx] - SIGNAL_MARGIN_DB : 0;
    uint16_t busy = 0;

    for (uint8_t i = 0; i < dwell; i++)
    {
        NRF_RADIO->EVENTS_RSSIEND = 0;
        NRF_RADIO->TASKS_RSSISTART = 1;
        if (!wait_event(&NRF_RADIO->EVENTS_RSSIEND))
            break;

        uint8_t v = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;
        if (v < strongest)
            strongest = v;
        if (v > weakest)
            weakest = v;
        if (v <= busy_thr)
            busy++;
    }

    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED);

    g_scan[idx].peak = strongest;
    g_scan[idx].weak = weakest;
    g_scan[idx].busy = (uint8_t)((busy * 255u) / dwell);

    if (busy)
        m_activity[idx] = ACTIVITY_HOLD_SWEEPS;
    else if (m_activity[idx])
        m_activity[idx]--;

    // Track the noise floor from the weakest sample of the visit. Attack fast
    // when the band gets quieter, rise slowly so a burst cannot drag it up.
    if (weakest > 0 && weakest < RSSI_INVALID)
    {
        if (weakest < g_floor[idx])
            g_floor[idx] = weakest;
        else if (g_floor[idx] < NOISE_FLOOR_MAX)
            g_floor[idx] = (uint8_t)((g_floor[idx] * 15 + weakest) / 16 + 1);
    }
    if (g_floor[idx] < NOISE_FLOOR_MIN)
        g_floor[idx] = NOISE_FLOOR_MIN;
    if (g_floor[idx] > NOISE_FLOOR_MAX)
        g_floor[idx] = NOISE_FLOOR_MAX;
}

void scanner_sweep(void)
{
    if (m_count == 0)
        return;

    radio_hfxo_start();

    // Samples per visit: the fixed dwell, or the adaptive split of the same
    // budget between active and quiet channels
    dwell_plan_t plan = {.quiet = m_dwell, .active = m_dwell};
    if (m_adaptive)
    {
        uint8_t active = 0;
        for (uint8_t i = 0; i < m_count; i++)
        {
            if (m_activity[i])
                active++;
        }
        plan = dwell_plan(m_dwell, m_count, active);
    }

    if (!m_shuffle)
    {
        for (uint8_t i = 0; i < m_count; i++)
            visit(i, m_activity[i] ? plan.active : plan.quiet);
    }
    else
    {
        // Visit in a rotating, interleaved order so a periodic emitter cannot
        // beat against the sweep period and hide from every pass. The stride
        // must be coprime with the channel count, otherwise the walk closes
        // early and leaves most of the band unvisited.
        m_lcg = sweep_order_next_seed(m_lcg);
        uint8_t stride = sweep_order_stride(m_count, m_lcg);
        uint8_t idx = (uint8_t)(m_lcg % m_count);
        for (uint8_t n = 0; n < m_count; n++)
        {
            visit(idx, m_activity[idx] ? plan.active : plan.quiet);
            idx = (uint8_t)((idx + stride) % m_count);
            if (n % 32 == 31)
                power_watchdog_feed();
        }
    }

    power_watchdog_feed();
}

uint8_t scanner_measure(uint16_t mhz, uint8_t samples)
{
    chan_tune_t t;
    if (!tune_for(mhz, &t))
        return RSSI_INVALID;

    radio_hfxo_start();
    radio_tune(&t);

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
        return RSSI_INVALID;

    uint8_t strongest = RSSI_INVALID;
    for (uint8_t i = 0; i < samples; i++)
    {
        NRF_RADIO->EVENTS_RSSIEND = 0;
        NRF_RADIO->TASKS_RSSISTART = 1;
        if (!wait_event(&NRF_RADIO->EVENTS_RSSIEND))
            break;

        uint8_t v = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;
        if (v < strongest)
            strongest = v;
    }

    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED);
    return strongest;
}

uint16_t scanner_park(uint16_t mhz, uint32_t window_ms, burst_t *out, uint16_t max_bursts,
                      park_stats_t *stats)
{
    chan_tune_t t;
    if (!tune_for(mhz, &t) || max_bursts == 0)
        return 0;

    radio_hfxo_start();
    radio_tune(&t);

    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY))
        return 0;

    // Trigger above the noise floor the sweep has tracked for this channel.
    // Learning it here from the first samples would take a steady carrier for
    // the floor and never see a burst on top of it.
    uint8_t floor_est = NOISE_FLOOR_INIT;
    for (uint8_t i = 0; i < m_count; i++)
    {
        if (m_mhz[i] == mhz)
        {
            floor_est = g_floor[i];
            break;
        }
    }
    if (floor_est < NOISE_FLOOR_MIN || floor_est > NOISE_FLOOR_MAX)
        floor_est = NOISE_FLOOR_INIT;

    uint8_t thr = (floor_est > SIGNAL_MARGIN_DB) ? floor_est - SIGNAL_MARGIN_DB : 0;

    uint32_t t_start = systime_us();
    uint32_t last_feed = t_start;
    uint32_t deadline_us = window_ms * 1000u;
    uint16_t count = 0;
    uint16_t dropped = 0;
    uint32_t on_us = 0;
    uint8_t peak_all = RSSI_INVALID;

    bool in_burst = false;
    uint32_t burst_start = 0;
    uint8_t burst_peak = RSSI_INVALID;
    uint32_t now = t_start;

    while ((now - t_start) < deadline_us)
    {
        NRF_RADIO->EVENTS_RSSIEND = 0;
        NRF_RADIO->TASKS_RSSISTART = 1;
        if (!wait_event(&NRF_RADIO->EVENTS_RSSIEND))
            break;

        uint8_t v = NRF_RADIO->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk;
        now = systime_us();

        if (v < peak_all)
            peak_all = v;

        bool active = (v <= thr);
        if (active && !in_burst)
        {
            in_burst = true;
            burst_start = now;
            burst_peak = v;
        }
        else if (active)
        {
            if (v < burst_peak)
                burst_peak = v;
        }
        else if (in_burst)
        {
            in_burst = false;
            uint32_t len = now - burst_start;
            on_us += len;
            if (count < max_bursts)
            {
                out[count].start_us = burst_start - t_start;
                out[count].len_us = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
                out[count].peak = burst_peak;
                count++;
            }
            else
            {
                dropped++;
            }
        }

        // Feed on elapsed time: a count based test stops feeding as soon as
        // bursts start being recorded
        if ((now - last_feed) >= PARK_WDT_FEED_US)
        {
            power_watchdog_feed();
            last_feed = now;
        }
    }

    // A burst still open when the window ends counts up to the last sample
    if (in_burst)
    {
        uint32_t len = now - burst_start;
        on_us += len;
        if (count < max_bursts)
        {
            out[count].start_us = burst_start - t_start;
            out[count].len_us = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
            out[count].peak = burst_peak;
            count++;
        }
        else
        {
            dropped++;
        }
    }

    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_event(&NRF_RADIO->EVENTS_DISABLED);

    if (stats)
    {
        stats->window_us = now - t_start;
        stats->on_us = on_us;
        stats->bursts = count;
        stats->dropped = dropped;
        stats->floor_rssi = floor_est;
        stats->peak_rssi = peak_all;
    }

    power_watchdog_feed();
    return count;
}
