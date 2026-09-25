// Host side test of the traffic classifier and the settings record format.
// The radio dependent parts are stubbed, everything else is the real code.
#include <stdio.h>
#include <string.h>

#include "classify.h"
#include "settings.h"

static uint8_t g_width = 1;
uint8_t classify_width(uint16_t mhz)
{
    (void)mhz;
    return g_width;
}

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got != want)
    {
        printf("  FAIL %-44s got %d want %d\n", what, got, want);
        failures++;
    }
    else
    {
        printf("  ok   %-44s\n", what);
    }
}

// Builds a burst train: count bursts of len_us every period_us
static uint16_t make_train(burst_t *out, uint16_t count, uint32_t period_us, uint16_t len_us,
                           park_stats_t *stats, uint32_t window_us)
{
    uint32_t on = 0;
    for (uint16_t i = 0; i < count; i++)
    {
        out[i].start_us = i * period_us;
        out[i].len_us = len_us;
        out[i].peak = 60;
        on += len_us;
    }
    memset(stats, 0, sizeof(*stats));
    stats->window_us = window_us;
    stats->on_us = on;
    stats->bursts = count;
    stats->peak_rssi = 60;
    return count;
}

// Deterministic pseudo-random numbers, so a failing case fails every time
static uint32_t g_seed = 1;
static uint32_t rnd(uint32_t range)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((g_seed >> 16) & 0x7FFF) % range;
}

// Appends a burst and books it in the stats
static void add_burst(burst_t *out, uint16_t *n, park_stats_t *stats, uint32_t start,
                      uint16_t len, uint8_t peak)
{
    out[*n].start_us = start;
    out[*n].len_us = len;
    out[*n].peak = peak;
    (*n)++;
    stats->on_us += len;
    stats->bursts = *n;
}

static void new_capture(park_stats_t *stats, uint32_t window_us)
{
    memset(stats, 0, sizeof(*stats));
    stats->window_us = window_us;
    stats->peak_rssi = 60;
}

// Spread sweeps: count channels, every one of them lit with the given odds in
// percent, plus the channel at index always (our own)
static void make_spread(uint8_t *hits, uint8_t count, uint8_t percent, int always)
{
    for (uint8_t i = 0; i < count; i++)
        hits[i] = rnd(100) < percent ? (uint8_t)(1 + rnd(3)) : 0;
    if (always >= 0)
        hits[always] = 5;
}

// Bluetooth BR/EDR on one of its 79 channels: every packet starts on the
// 625us slot grid, the channel comes back after a pseudo-random number of slots
static uint16_t make_bt_classic(burst_t *out, park_stats_t *stats, bool on_grid)
{
    uint16_t n = 0;
    uint32_t t = 3000;
    new_capture(stats, 2000000);
    while (n < 40)
    {
        uint16_t len = (n % 5 == 4) ? 1622 : 366; // DH1, now and then a DH3
        int jitter = (int)rnd(17) - 8;
        add_burst(out, &n, stats, (uint32_t)((int)t + jitter), len, 55);
        uint32_t slots = 8 + rnd(110);
        t += on_grid ? slots * 625u : slots * 625u + 100u + rnd(420);
    }
    return n;
}

int main(void)
{
    burst_t bursts[256];
    park_stats_t stats;
    classify_features_t f;
    uint8_t conf = 0;

    printf("classifier\n");

    // A WiFi access point that is only beaconing: wide, sparse, 102.4ms apart
    g_width = 18;
    uint16_t n = make_train(bursts, 20, 102400, 1200, &stats, 2000000);
    classify_features(2437, bursts, n, &stats, &f);
    check("beacon period detected (ms)", (int)(f.period_us / 1000), 102);
    check("beacon -> WIFI BEACON", classify_decide(&f, &conf), VERDICT_WIFI_BEACON);

    // Busy WiFi: same width, much higher duty, no clean cadence
    for (uint16_t i = 0; i < 60; i++)
    {
        bursts[i].start_us = i * 9000 + (i % 7) * 900; // jittered
        bursts[i].len_us = 3000;
        bursts[i].peak = 55;
    }
    memset(&stats, 0, sizeof(stats));
    stats.window_us = 600000;
    stats.on_us = 60 * 3000;
    stats.bursts = 60;
    stats.peak_rssi = 55;
    classify_features(2437, bursts, 60, &stats, &f);
    check("busy wifi -> WIFI", classify_decide(&f, &conf), VERDICT_WIFI);
    check("duty around 30 percent", (int)(f.duty_ppm / 10000), 30);

    // Microwave oven: mains locked, wide, half the time on, very strong
    g_width = 20;
    n = make_train(bursts, 40, 20000, 9000, &stats, 800000);
    stats.peak_rssi = 40;
    classify_features(2450, bursts, n, &stats, &f);
    check("mains lock detected", f.mains_locked, 1);
    check("oven -> MICROWAVE", classify_decide(&f, &conf), VERDICT_MICROWAVE);
    check("oven confidence >= 85", conf >= 85, 1);

    // BLE advertiser on 2402: narrow, short, ~100ms apart
    g_width = 2;
    n = make_train(bursts, 20, 100000, 400, &stats, 2000000);
    classify_features(2402, bursts, n, &stats, &f);
    check("ble channel recognised", f.on_ble_channel, 1);
    check("advertiser -> BLE ADV", classify_decide(&f, &conf), VERDICT_BLE_ADV);

    // A carrier: always on
    g_width = 3;
    memset(&stats, 0, sizeof(stats));
    stats.window_us = 1000000;
    stats.on_us = 990000;
    stats.bursts = 1;
    bursts[0].start_us = 0;
    bursts[0].len_us = 65535;
    classify_features(2440, bursts, 1, &stats, &f);
    check("carrier -> CONTINUOUS", classify_decide(&f, &conf), VERDICT_CONTINUOUS);

    // Nothing at all
    memset(&stats, 0, sizeof(stats));
    stats.window_us = 1000000;
    classify_features(2440, bursts, 0, &stats, &f);
    check("empty air -> QUIET", classify_decide(&f, &conf), VERDICT_QUIET);

    // Few bursts must lower the confidence, never raise a claim
    g_width = 18;
    n = make_train(bursts, 5, 102400, 1200, &stats, 600000);
    classify_features(2437, bursts, n, &stats, &f);
    uint8_t weak_conf = 0;
    classify_decide(&f, &weak_conf);
    check("short capture lowers confidence", weak_conf <= 60, 1);

    uint8_t hits[141];
    verdict_t v;

    // Bluetooth classic: slot grid here, pseudo-random hops over 79 channels
    printf("\nbluetooth classic\n");
    g_width = 1;
    g_seed = 7;
    n = make_bt_classic(bursts, &stats, true);
    make_spread(hits, 84, 60, 41);
    classify_run(2441, bursts, n, &stats, hits, 84, &v);
    check("bt slots on the 625us grid >= 90 pct", v.f.slot_pct >= 90, 1);
    check("bt hops seen over the band", v.f.spread_chans >= 16 && v.f.spread_span >= 40, 1);
    check("bt -> BT CLASSIC", v.kind, VERDICT_BT_CLASSIC);

    // Near miss: same density and hops, but the gaps ignore the slot grid
    g_seed = 7;
    n = make_bt_classic(bursts, &stats, false);
    make_spread(hits, 84, 60, 41);
    classify_run(2441, bursts, n, &stats, hits, 84, &v);
    check("off-grid gaps: slot share low", v.f.slot_pct < 40, 1);
    check("off-grid gaps -> not BT CLASSIC", v.kind != VERDICT_BT_CLASSIC, 1);

    // Near miss: on the grid, but nothing else in the band moves
    g_seed = 7;
    n = make_bt_classic(bursts, &stats, true);
    make_spread(hits, 84, 0, 41);
    classify_run(2441, bursts, n, &stats, hits, 84, &v);
    check("grid without hops -> not BT CLASSIC", v.kind != VERDICT_BT_CLASSIC, 1);

    // ANT+ heart rate strap: 8070/32768s = 246.28ms on 2457MHz, one channel
    printf("\nant\n");
    g_width = 1;
    g_seed = 11;
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 8; i++)
        add_burst(bursts, &n, &stats, 40000 + i * 246277u + rnd(40), 180, 62);
    make_spread(hits, 84, 0, 57);
    classify_run(2457, bursts, n, &stats, hits, 84, &v);
    check("ant cadence found (ms)", (int)(v.f.fixed_period_us / 1000), 246);
    check("ant cadence shown as the period (ms)", (int)(v.f.period_us / 1000), 246);
    check("ant+ on 2457 -> ANT", v.kind, VERDICT_ANT);
    check("ant+ confidence >= 70", v.confidence >= 70, 1);

    // One missed packet leaves a double gap and must not break the cadence
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 8; i++)
    {
        if (i != 4)
            add_burst(bursts, &n, &stats, 40000 + i * 250000u + rnd(40), 180, 62);
    }
    classify_run(2466, bursts, n, &stats, hits, 84, &v);
    check("4Hz with a missed packet -> ANT", v.kind, VERDICT_ANT);
    check("ant off 2457 is less certain", v.confidence < 70, 1);

    // Near miss: a BLE style 250ms interval with 0..10ms advDelay jitter
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 8; i++)
        add_burst(bursts, &n, &stats, 40000 + i * 250000u + rnd(10000), 180, 62);
    classify_run(2457, bursts, n, &stats, hits, 84, &v);
    check("jittered 250ms -> not ANT", v.kind != VERDICT_ANT, 1);

    // Analogue video sender: wide, never off, level steady. Each window holds
    // one burst clipped at 65535us; the windows start a few ms apart.
    printf("\nanalogue video\n");
    g_width = 14;
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 8; i++)
        add_burst(bursts, &n, &stats, i * 253100u, 0xFFFF, (uint8_t)(48 + i % 3));
    stats.on_us = 1995000;
    classify_run(2432, bursts, n, &stats, NULL, 0, &v);
    check("video: no long gaps", v.f.long_gaps, 0);
    check("video -> ANALOG VIDEO", v.kind, VERDICT_VIDEO);

    // Near miss: wide and 91 percent on, but it drops out every 33ms
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 60; i++)
        add_burst(bursts, &n, &stats, i * 33000u, 30000, 48);
    stats.window_us = 1980000;
    classify_run(2432, bursts, n, &stats, NULL, 0, &v);
    check("dropouts counted as long gaps", v.f.long_gaps > v.f.seams, 1);
    check("wide with dropouts -> CONTINUOUS", v.kind, VERDICT_CONTINUOUS);

    // Near miss: wide and gapless, but the level wanders by 20dB
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 8; i++)
        add_burst(bursts, &n, &stats, i * 253100u, 0xFFFF, (uint8_t)(45 + (i % 2) * 20));
    stats.on_us = 1995000;
    classify_run(2432, bursts, n, &stats, NULL, 0, &v);
    check("wide with a wandering level -> CONTINUOUS", v.kind, VERDICT_CONTINUOUS);

    // RC link, pseudo-random hops (ExpressLRS style): 4ms frames, our channel
    // comes back after a varying number of them
    printf("\nrc fhss\n");
    g_width = 1;
    g_seed = 23;
    new_capture(&stats, 2000000);
    n = 0;
    {
        uint32_t t = 5000;
        while (n < 30)
        {
            add_burst(bursts, &n, &stats, t + rnd(20), 350, 58);
            t += (2 + rnd(14)) * 4000u;
        }
    }
    make_spread(hits, 84, 25, 20);
    classify_run(2420, bursts, n, &stats, hits, 84, &v);
    check("rc frame found (ms)", (int)((v.f.frame_us + 500) / 1000), 4);
    check("rc frame multiples vary", v.f.frame_multiples >= 2, 1);
    check("pseudo-random hopper -> RC FHSS", v.kind, VERDICT_RC_FHSS);

    // Cyclic sequence (FlySky AFHDS 2A style): 16 channels x 3.85ms frames
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 32; i++)
        add_burst(bursts, &n, &stats, 2000 + i * 61600u + rnd(20), 500, 58);
    make_spread(hits, 84, 20, 20);
    classify_run(2420, bursts, n, &stats, hits, 84, &v);
    check("cyclic hopper -> RC FHSS", v.kind, VERDICT_RC_FHSS);

    // Near miss: a fixed-channel sender every 8ms (a ShockBurst mouse), even
    // with other activity in the band
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 200; i++)
        add_burst(bursts, &n, &stats, i * 8000u + rnd(20), 250, 58);
    make_spread(hits, 84, 25, 20);
    classify_run(2420, bursts, n, &stats, hits, 84, &v);
    check("8ms fixed channel: frame multiple is 1", v.f.frame_ratio, 1);
    check("fixed channel sender -> NARROW BURST", v.kind, VERDICT_NARROW_BURST);

    // Near miss: a fixed 300ms cadence with one missed packet looks like two
    // frame multiples (12 and 24 of 25ms). A few other emitters in the band
    // must not turn it into a hopper: the cadence explains every gap.
    new_capture(&stats, 2000000);
    n = 0;
    for (uint16_t i = 0; i < 7; i++)
    {
        if (i != 3)
            add_burst(bursts, &n, &stats, 1000 + i * 300000u + rnd(20), 300, 58);
    }
    memset(hits, 0, sizeof(hits));
    hits[20] = 5;
    hits[40] = 1;
    hits[60] = 2;
    hits[70] = 1;
    classify_run(2420, bursts, n, &stats, hits, 84, &v);
    check("cadence with a missed packet -> not RC FHSS", v.kind != VERDICT_RC_FHSS, 1);

    // Names on screen
    check("every verdict has a name", strcmp(classify_name(VERDICT_RC_FHSS), "UNKNOWN") != 0 &&
                                          strcmp(classify_name(VERDICT_BT_CLASSIC), "UNKNOWN") != 0 &&
                                          strcmp(classify_name(VERDICT_ANT), "UNKNOWN") != 0 &&
                                          strcmp(classify_name(VERDICT_VIDEO), "UNKNOWN") != 0,
          1);

    printf("\nsettings\n");
    settings_defaults();
    uint32_t crc_a = settings_crc32(&g_settings, sizeof(g_settings));
    g_settings.contrast ^= 0x01;
    uint32_t crc_b = settings_crc32(&g_settings, sizeof(g_settings));
    check("crc changes with the payload", crc_a != crc_b, 1);

    settings_defaults();
    check("default band is ISM", g_settings.band, BAND_ISM);
    check("default dwell is the fast sweep value", g_settings.dwell, SCAN_DWELL_SAMPLES_DEFAULT);
    check("waterfall defaults to shades", g_settings.wf_mode, WF_DITHER);

    g_settings.contrast = 42;
    settings_save();
    g_settings.contrast = 0;
    settings_load();
    check("settings survive a save/load round trip", g_settings.contrast, 42);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
