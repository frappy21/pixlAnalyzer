#include <string.h>

#include "radar.h"

void radar_init(radar_work_t *w)
{
    if (!w)
        return;
    memset(w, 0, sizeof(*w));
}

// A channel is active when its busy share or its peak above the floor says
// so: busy is the scanner's "share of samples above the floor" (0..255),
// peak_db is the dB above the floor the sweep saw.
static bool channel_active(uint8_t busy, uint8_t peak, uint8_t floor)
{
    (void)floor;
    if (busy == 0)
        return false;
    return busy > 63 || peak > 0;
}

void radar_sweep(radar_work_t *w, const uint8_t *busy, const uint8_t *peak,
                 const uint8_t *floor)
{
    if (!w || !busy || !peak || !floor)
        return;

    for (uint8_t c = 0; c < RADAR_CHANS; c++)
    {
        // dB above the floor, capped
        uint16_t db = (uint16_t)(floor[c] - peak[c]);
        if (db > 90)
            db = 90;

        w->busy_sum[c] += busy[c];
        if (db > w->peak_sum[c])
            w->peak_sum[c] = db;

        if (channel_active(busy[c], peak[c], floor[c]))
            w->busy_hist[c] = (uint8_t)((w->busy_hist[c] << 1) | 1);
    }
    w->sweeps++;

    if (w->sweeps < RADAR_WINDOW_SWEEPS)
        return;

    // Close the window
    w->sweeps = 0;
    for (uint8_t c = 0; c < RADAR_CHANS; c++)
    {
        // The scanner's busy is a 0..255 share; keep it as a percent so the
        // thresholds below read like duty cycles
        uint8_t pct255 = (uint8_t)(w->busy_sum[c] / RADAR_WINDOW_SWEEPS);
        uint8_t pct = (uint8_t)((uint32_t)pct255 * 100u / 255u);
        uint8_t db = (uint8_t)w->peak_sum[c];
        w->busy_sum[c] = 0;
        w->peak_sum[c] = 0;

        bool active = pct > 5 || db >= RADAR_BUSY_MARGIN_DB;
        if (active)
        {
            w->busy_pct[c] = w->busy_pct[c] ? (uint8_t)((w->busy_pct[c] + pct) / 2) : pct;
            if (db > w->peak_db[c])
                w->peak_db[c] = db;
            if (w->floor[c] == 0)
                w->floor[c] = floor[c];
        }
    }
    for (uint8_t c = 0; c < RADAR_CHANS; c++)
        w->busy_hist[c] = (uint8_t)(w->busy_hist[c] << 1);
}

static uint8_t window_count(uint8_t hist)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < 8; i++)
        if (hist & (1 << i))
            n++;
    return n;
}

uint16_t radar_active_chans(const radar_work_t *w)
{
    if (!w)
        return 0;
    uint16_t n = 0;
    for (uint8_t c = 0; c < RADAR_CHANS; c++)
        if (window_count(w->busy_hist[c]))
            n++;
    return n;
}

// ---------------------------------------------------------------------------
// Signal table
// ---------------------------------------------------------------------------

static uint8_t signal_width(const radar_work_t *w, uint8_t c)
{
    // The full contiguous busy run around the channel, unbounded: a wide
    // WiFi channel is one signal, not a series of MHz wide ones
    uint8_t lo = c;
    while (lo > 0 && window_count(w->busy_hist[lo - 1]))
        lo--;
    uint8_t hi = c;
    while (hi + 1 < RADAR_CHANS && window_count(w->busy_hist[hi + 1]))
        hi++;
    return (uint8_t)(hi - lo + 1);
}

// The run around c, as its bounds
static void signal_run(const radar_work_t *w, uint8_t c, uint8_t *lo, uint8_t *hi)
{
    uint8_t l = c;
    while (l > 0 && window_count(w->busy_hist[l - 1]))
        l--;
    uint8_t h = c;
    while (h + 1 < RADAR_CHANS && window_count(w->busy_hist[h + 1]))
        h++;
    *lo = l;
    *hi = h;
}

// The centre of a run: the strongest channel, the middle when several are
// equally strong (the usual case for an unmodulated carrier)
static uint8_t signal_centre(const radar_work_t *w, uint8_t lo, uint8_t hi)
{
    uint8_t best = lo;
    for (uint8_t i = lo + 1; i <= hi; i++)
    {
        if (w->peak_db[i] > w->peak_db[best] ||
            (w->peak_db[i] == w->peak_db[best] && w->busy_pct[i] > w->busy_pct[best]))
            best = i;
    }
    if (w->peak_db[best] == w->peak_db[lo] && best == lo)
    {
        // A flat run: its middle
        return (uint8_t)((lo + hi) / 2);
    }
    return best;
}

static radar_kind_t classify_signal(const radar_work_t *w, uint8_t c)
{
    uint8_t busy = w->busy_pct[c];
    uint8_t width = signal_width(w, c);
    uint16_t active = radar_active_chans(w);

    // Many channels at once with little duty each: a hopper
    if (active >= 10 && busy < 40)
        return RADAR_HOPPER;

    // Gapless and wide: analogue video (AV sender, camera, baby monitor)
    if (busy >= 85 && width >= 2)
        return RADAR_VIDEO;

    // One wide channel nearly full: WiFi video or a strong AP
    if (busy >= 70 && width >= 3)
        return RADAR_WIFI;

    // Short bursts: a control link
    if (busy < 30)
        return RADAR_CONTROL;

    // Steady narrow: a carrier
    return RADAR_CARRIER;
}

void radar_signals_rebuild(radar_work_t *w)
{
    if (!w)
        return;

    w->n_sig = 0;

    // Candidate channels: persistent over the recent windows
    for (uint8_t c = 0; c < RADAR_CHANS && w->n_sig < RADAR_MAX_SIGNALS; c++)
    {
        uint8_t wins = window_count(w->busy_hist[c]);
        if (wins < RADAR_PERSIST_WINDOWS)
            continue;
        if (w->busy_pct[c] == 0 && w->peak_db[c] < RADAR_BUSY_MARGIN_DB)
            continue;

        // The whole contiguous run is one signal
        uint8_t lo, hi;
        signal_run(w, c, &lo, &hi);
        uint8_t centre = signal_centre(w, lo, hi);

        // Skip runs an already accepted signal covers
        bool covered = false;
        for (uint8_t s = 0; s < w->n_sig; s++)
        {
            int d = (int)w->sig[s].mhz - (int)(RADAR_START_MHZ + c);
            if (d < 0)
                d = -d;
            if (d < w->sig[s].width_mhz)
            {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;

        radar_signal_t *sig = &w->sig[w->n_sig++];
        sig->mhz = (uint16_t)(RADAR_START_MHZ + centre);
        sig->peak_db = w->peak_db[centre];
        sig->busy_pct = w->busy_pct[centre] > 99 ? 99 : w->busy_pct[centre];
        sig->width_mhz = (uint8_t)(hi - lo + 1);
        sig->windows = wins;
        sig->kind = classify_signal(w, centre);
        sig->first_seen = false;

        if (w->hunting)
        {
            // New when the run's channels were quiet at baseline
            bool any_known = false;
            for (uint8_t i = lo; i <= hi; i++)
                if (w->baseline_pct[i] >= 8)
                    any_known = true;
            if (!any_known)
                sig->first_seen = true;
        }

        // The rest of the run belongs to this signal now
        c = hi;
    }

    // Strongest first
    for (uint8_t i = 0; i + 1 < w->n_sig; i++)
    {
        for (uint8_t j = i + 1; j < w->n_sig; j++)
        {
            uint16_t a = (uint16_t)(w->sig[i].peak_db * 100u + w->sig[i].busy_pct);
            uint16_t b = (uint16_t)(w->sig[j].peak_db * 100u + w->sig[j].busy_pct);
            if (b > a)
            {
                radar_signal_t tmp = w->sig[i];
                w->sig[i] = w->sig[j];
                w->sig[j] = tmp;
            }
        }
    }

    w->hop_chans = (uint8_t)(radar_active_chans(w) & 0xFF);
}

uint8_t radar_signals(const radar_work_t *w)
{
    return w ? w->n_sig : 0;
}

const radar_signal_t *radar_signal(const radar_work_t *w, uint8_t index)
{
    if (!w || index >= w->n_sig)
        return 0;
    return &w->sig[index];
}

void radar_hunt_arm(radar_work_t *w)
{
    if (!w)
        return;
    w->hunting = true;
    for (uint8_t c = 0; c < RADAR_CHANS; c++)
        w->baseline_pct[c] = w->busy_pct[c];
    // Everything already in the table is known: forget the flags
    for (uint8_t s = 0; s < w->n_sig; s++)
        w->sig[s].first_seen = false;
}

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

const char *radar_kind_name(radar_kind_t kind)
{
    switch (kind)
    {
    case RADAR_VIDEO:
        return "ANALOG VIDEO";
    case RADAR_WIFI:
        return "WIFI VIDEO";
    case RADAR_HOPPER:
        return "HOPPING";
    case RADAR_CONTROL:
        return "CONTROL LINK";
    case RADAR_CARRIER:
        return "CARRIER";
    default:
        return "?";
    }
}

bool radar_microwave(const burst_t *bursts, uint16_t n, const park_stats_t *stats,
                     uint8_t floor_db, int8_t *out_level, const char **out_verdict)
{
    if (!stats || !out_level)
        return false;

    (void)bursts;
    (void)n;

    // The level above the floor, in dB: 0 = at the floor
    int16_t db = (int16_t)floor_db - (int16_t)stats->peak_rssi;
    if (db < 0)
        db = 0;
    if (db > 99)
        db = 99;
    *out_level = (int8_t)db;

    if (out_verdict)
    {
        // A leaky oven shows as a strong wide carrier wobbling with the
        // mains. Without demodulation this is a level statement, not a
        // safety certificate: the screen says so itself.
        if (db >= 30)
            *out_verdict = "VERY STRONG 2450";
        else if (db >= 20)
            *out_verdict = "STRONG: CHECK OVEN";
        else if (db >= 10)
            *out_verdict = "ELEVATED LEVEL";
        else
            *out_verdict = "FLOOR LEVEL";
    }
    return true;
}
