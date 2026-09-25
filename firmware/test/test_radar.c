// Host side test of the radar bookkeeping: window accumulation, the
// signal table, the verdict kinds and the hunt baseline logic.
#include <stdio.h>
#include <string.h>

#include "radar.h"

static int failures = 0;

static void check(const char *what, long got, long want)
{
    if (got != want)
    {
        printf("  FAIL %-44s got %ld want %ld\n", what, got, want);
        failures++;
    }
    else
    {
        printf("  ok   %-44s\n", what);
    }
}

static void check_true(const char *what, bool got)
{
    check(what, got ? 1 : 0, 1);
}

#define CHANS RADAR_CHANS

// One quiet band: busy 0 everywhere, floor ~ 100 (-100 dBm), peak at floor
static void quiet(uint8_t *busy, uint8_t *peak, uint8_t *floor)
{
    memset(busy, 0, CHANS);
    for (uint8_t i = 0; i < CHANS; i++)
    {
        floor[i] = 100;
        peak[i] = 100;
    }
}

// A strong gapless video carrier on channel 55: busy 255, peak 40 dB above
// the floor, three channels wide (54..56)
static void video(uint8_t *busy, uint8_t *peak)
{
    for (uint8_t c = 54; c <= 56; c++)
    {
        busy[c] = 255;
        peak[c] = 40;
    }
}

// A WiFi-like channel: busy 200, peak 50, channels 30..36
static void wifi(uint8_t *busy, uint8_t *peak)
{
    for (uint8_t c = 30; c <= 36; c++)
    {
        busy[c] = 200;
        peak[c] = 50;
    }
}

static void test_video(void)
{
    printf("radar: video verdict\n");

    radar_work_t w;
    radar_init(&w);

    uint8_t busy[CHANS], peak[CHANS], floor[CHANS];
    quiet(busy, peak, floor);

    // Two quiet sweeps, then the video long enough for clean windows
    for (int i = 0; i < 14; i++)
    {
        if (i >= 2)
            video(busy, peak);
        radar_sweep(&w, busy, peak, floor);
    }
    radar_signals_rebuild(&w);

    check("one signal", radar_signals(&w), 1);
    const radar_signal_t *s = radar_signal(&w, 0);
    check_true("signal exists", s != 0);
    if (s)
    {
        check("video centre", s->mhz, 2400 + 55);
        check("video kind", s->kind, RADAR_VIDEO);
        check_true("video duty", s->busy_pct >= 85);
        check("video width", s->width_mhz, 3);
    }
}

static void test_wifi(void)
{
    printf("radar: wifi verdict\n");

    radar_work_t w;
    radar_init(&w);

    uint8_t busy[CHANS], peak[CHANS], floor[CHANS];
    quiet(busy, peak, floor);

    for (int i = 0; i < 12; i++)
    {
        wifi(busy, peak);
        radar_sweep(&w, busy, peak, floor);
    }
    radar_signals_rebuild(&w);

    check("one signal", radar_signals(&w), 1);
    const radar_signal_t *s = radar_signal(&w, 0);
    check_true("signal exists", s != 0);
    if (s)
        check("wifi kind", s->kind, RADAR_WIFI);
}

static void test_quiet(void)
{
    printf("radar: quiet band\n");

    radar_work_t w;
    radar_init(&w);

    uint8_t busy[CHANS], peak[CHANS], floor[CHANS];
    quiet(busy, peak, floor);
    for (int i = 0; i < 8; i++)
        radar_sweep(&w, busy, peak, floor);
    radar_signals_rebuild(&w);

    check("no signals", radar_signals(&w), 0);
    check("active channels", radar_active_chans(&w), 0);
}

static void test_hunt(void)
{
    printf("radar: hunt\n");

    radar_work_t w;
    radar_init(&w);

    uint8_t busy[CHANS], peak[CHANS], floor[CHANS];
    quiet(busy, peak, floor);

    // The baseline: a known WiFi channel up
    wifi(busy, peak);
    for (int i = 0; i < 4; i++)
        radar_sweep(&w, busy, peak, floor);
    radar_signals_rebuild(&w);
    radar_hunt_arm(&w);

    // The known signal stays known
    for (int i = 0; i < 4; i++)
        radar_sweep(&w, busy, peak, floor);
    radar_signals_rebuild(&w);
    const radar_signal_t *s = radar_signal(&w, 0);
    check_true("known signal not new", s && !s->first_seen);

    // A video sender appears after the baseline
    video(busy, peak);
    for (int i = 0; i < 4; i++)
        radar_sweep(&w, busy, peak, floor);
    radar_signals_rebuild(&w);

    bool found_new = false;
    for (uint8_t i = 0; i < radar_signals(&w); i++)
    {
        const radar_signal_t *sig = radar_signal(&w, i);
        if (sig->mhz == 2400 + 55)
        {
            check_true("new signal flagged", sig->first_seen);
            found_new = true;
        }
    }
    check_true("the video signal is in the table", found_new);
}

static void test_names(void)
{
    printf("radar: names\n");

    check("video name", (long)(size_t)radar_kind_name(RADAR_VIDEO) != 0, 1);
    check_true("analog video named",
               strcmp(radar_kind_name(RADAR_VIDEO), "ANALOG VIDEO") == 0);
    check_true("hopping named", strcmp(radar_kind_name(RADAR_HOPPER), "HOPPING") == 0);
}

int main(void)
{
    test_video();
    test_wifi();
    test_quiet();
    test_hunt();
    test_names();

    if (failures)
    {
        printf("radar: %d failures\n", failures);
        return 1;
    }
    printf("radar: all checks passed\n");
    return 0;
}
