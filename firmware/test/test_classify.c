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
