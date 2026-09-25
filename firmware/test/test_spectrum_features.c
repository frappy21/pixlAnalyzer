// Host side test of the spectrum model features: trace modes, RBW 2 MHz, peak
// search, occupancy, calibration, the plot at any height, the dBm labels and
// the adaptive dwell budget. spectrum.c is included so its state can be set
// directly; the scanner and the settings are stubbed, gfx is the real code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/spectrum.c"
#include "dwell.h"

uint8_t g_frame_buffer[DISP_BUF_SIZE];
settings_data_t g_settings;
chan_result_t g_scan[SCAN_MAX_CHANNELS];
uint8_t g_floor[SCAN_MAX_CHANNELS];

static uint8_t m_stub_count;
static uint16_t m_stub_lo = 2400;

uint8_t scanner_count(void) { return m_stub_count; }
uint16_t scanner_span_start(void) { return m_stub_lo; }
uint16_t scanner_span_end(void) { return m_stub_count ? (uint16_t)(m_stub_lo + m_stub_count - 1) : 0; }
uint16_t scanner_mhz(uint8_t index) { return (uint16_t)(m_stub_lo + index); }

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                                           \
    do                                                                                             \
    {                                                                                              \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
        {                                                                                          \
            failures++;                                                                            \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static uint32_t rnd(void)
{
    static uint32_t s = 0x1234567u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Sets every channel's reading so db_above_floor() returns db (fixed floor)
static void set_live(const uint8_t *db, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        g_scan[i].peak = (uint8_t)(NOISE_FLOOR_INIT - db[i]);
        g_floor[i] = NOISE_FLOOR_INIT;
    }
}

static void fresh(uint8_t count)
{
    m_stub_count = count;
    m_stub_lo = 2400;
    memset(&g_settings, 0, sizeof(g_settings)); // fixed floor, wf_decim 0
    m_trace = TRACE_PEAK;
    m_rbw = 1;
    m_cal_db = 0;
    spectrum_reset();
}

// ---------------------------------------------------------------------------

static void test_dwell(void)
{
    int bad = 0;
    for (int base = 1; base <= SCAN_DWELL_SAMPLES_MAX; base++)
    {
        for (int count = 1; count <= SCAN_MAX_CHANNELS; count++)
        {
            for (int active = 0; active <= count; active++)
            {
                dwell_plan_t p = dwell_plan((uint8_t)base, (uint8_t)count, (uint8_t)active);
                uint32_t total = (uint32_t)(count - active) * p.quiet + (uint32_t)active * p.active;

                // Never more samples per sweep than the fixed dwell: the sweep
                // rate cannot drop below what it is today
                bool ok = total <= (uint32_t)count * base;
                ok &= p.quiet >= 1 && p.quiet <= base;
                ok &= p.active >= base && p.active <= SCAN_DWELL_SAMPLES_MAX;
                if (active == 0 || active == count)
                    ok &= p.quiet == base && p.active == base;
                if (!ok && bad++ < 5)
                    printf("  FAIL dwell base %d count %d active %d: quiet %d active %d\n", base,
                           count, active, p.quiet, p.active);
                checks++;
            }
        }
    }
    failures += bad;

    // The typical case: a few busy channels on the ISM band get a lot more
    dwell_plan_t p = dwell_plan(32, 84, 10);
    CHECK(p.quiet == 16 && p.active > 100, "dwell 32/84/10: quiet %d active %d", p.quiet,
          p.active);
}

static void test_rbw(void)
{
    CHECK(rbw2_combine(20, 20) == 20, "equal halves keep their level");
    CHECK(rbw2_combine(20, 0) == 17 && rbw2_combine(0, 20) == 17,
          "narrowband loses 3 dB against the doubled noise");
    CHECK(rbw2_combine(0, 0) == 0, "noise stays at the floor");
    CHECK(rbw2_combine(10, 8) == 9, "close halves: power mean");

    // Pairs start at an even MHz: an odd span start leaves the first alone
    fresh(6);
    m_stub_lo = 2401;
    uint8_t db[6] = {10, 20, 0, 30, 30, 5};
    set_live(db, 6);
    spectrum_set_rbw(2);
    spectrum_update(1000);
    CHECK(m_live_db[0] == 10, "2401 has no partner below it: %d", m_live_db[0]);
    CHECK(m_live_db[1] == 17 && m_live_db[2] == 17, "2402+2403: %d %d", m_live_db[1],
          m_live_db[2]);
    CHECK(m_live_db[3] == 30 && m_live_db[4] == 30, "2404+2405: %d %d", m_live_db[3],
          m_live_db[4]);
    CHECK(m_live_db[5] == 5, "2406 without 2407 stays: %d", m_live_db[5]);

    // Not adjacent channels (BLE advertising preset) are never combined
    spectrum_set_rbw(1);
    spectrum_update(1100);
    CHECK(m_live_db[1] == 20 && m_live_db[2] == 0, "RBW 1 leaves the readings alone");
}

static void test_traces(void)
{
    fresh(4);
    uint8_t db[4] = {0, 20, 40, 10};
    set_live(db, 4);

    // The average creeps up to a steady level
    spectrum_set_trace(TRACE_AVG);
    for (int i = 0; i < 200; i++)
        spectrum_update(1000 + (uint32_t)i);
    CHECK(spectrum_trace_db(1) == 20 && spectrum_trace_db(2) == 40,
          "average settles on a steady level: %d %d", spectrum_trace_db(1), spectrum_trace_db(2));

    // One burst moves it a sixteenth of the way
    uint8_t burst[4] = {32, 20, 40, 10};
    set_live(burst, 4);
    spectrum_update(1300);
    CHECK(spectrum_trace_db(0) == 2, "average after one burst of 32: %d", spectrum_trace_db(0));

    // Min hold keeps the lowest level since it was selected
    spectrum_set_trace(TRACE_MIN);
    uint8_t lows[4] = {5, 10, 40, 10};
    set_live(lows, 4);
    spectrum_update(1400);
    set_live(db, 4);
    spectrum_update(1500);
    CHECK(spectrum_trace_db(0) == 0 && spectrum_trace_db(1) == 10 && spectrum_trace_db(2) == 40,
          "min hold: %d %d %d", spectrum_trace_db(0), spectrum_trace_db(1), spectrum_trace_db(2));
    CHECK(spectrum_max_db(0) == 32, "max hold unaffected by the trace mode");

    spectrum_clear_max();
    CHECK(spectrum_trace_db(1) == 0 && spectrum_max_db(0) == 0, "clear max restarts both holds");

    // Peak mode is the old trace
    spectrum_set_trace(TRACE_PEAK);
    CHECK(spectrum_trace_db(2) == m_peak_db[2], "peak mode shows the peak hold");
}

static void test_levels(void)
{
    fresh(4);
    uint8_t db[4] = {0, 20, 0, 0};
    set_live(db, 4);
    spectrum_update(1000);

    int dbm = 0;
    spectrum_set_cal(4);
    CHECK(spectrum_level_dbm(1, &dbm) && dbm == -(NOISE_FLOOR_INIT - 20) + 4,
          "peak mode: live reading plus the offset, got %d", dbm);
    CHECK(spectrum_dbm(70) == -66, "dbm of 70 with +4: %d", spectrum_dbm(70));

    spectrum_set_cal(-30);
    CHECK(spectrum_cal() == SPECTRUM_CAL_MIN_DB, "offset clamps low: %d", spectrum_cal());
    spectrum_set_cal(30);
    CHECK(spectrum_cal() == SPECTRUM_CAL_MAX_DB, "offset clamps high: %d", spectrum_cal());
    spectrum_set_cal(0);

    // Min hold of 20 over a floor of -92 dBm reads -72
    spectrum_set_trace(TRACE_MIN);
    spectrum_update(1100);
    CHECK(spectrum_level_dbm(1, &dbm) && dbm == -72, "min trace level: %d", dbm);

    g_scan[3].peak = RSSI_INVALID;
    spectrum_set_trace(TRACE_PEAK);
    CHECK(!spectrum_level_dbm(3, &dbm), "no reading, no level");
    CHECK(!spectrum_level_dbm(9, &dbm), "channel out of range, no level");

    // The mean tracked floor labels the grid when auto floor is on
    g_settings.auto_floor = 1;
    g_floor[0] = 80;
    g_floor[1] = 90;
    g_floor[2] = 90;
    g_floor[3] = 100;
    CHECK(spectrum_floor_rssi() == 90, "mean floor: %d", spectrum_floor_rssi());
    g_settings.auto_floor = 0;
    CHECK(spectrum_floor_rssi() == NOISE_FLOOR_INIT, "fixed floor");
}

static void test_peaks(void)
{
    fresh(84);

    // A ragged 20 MHz wide block (WiFi), a narrow strong carrier, a weak
    // blip that is only noise, and a lower narrow peak on the block's flank
    uint8_t *v = m_peak_db;
    for (int i = 10; i < 30; i++)
        v[i] = (uint8_t)(20 + (i % 3)); // 20..22 ripple
    v[30] = 10;
    v[31] = 12; // shoulder on the flank, not prominent
    v[50] = 35;
    v[51] = 10;
    v[60] = 4; // below PEAK_MIN_DB
    v[70] = 15;
    v[71] = 15; // plateau: one peak at 70

    uint8_t idx;
    CHECK(spectrum_find_peak(0, &idx) && idx == 50, "strongest peak: %d", idx);
    CHECK(spectrum_find_peak(1, &idx) && idx >= 10 && idx < 30, "the block is next: %d", idx);
    CHECK(spectrum_find_peak(2, &idx) && idx == 70, "then the plateau, at its start: %d", idx);
    CHECK(!spectrum_find_peak(3, &idx), "the ripple and the blip are no peaks");

    fresh(84);
    CHECK(!spectrum_find_peak(0, &idx), "a quiet band has no peak");

    // Two equal peaks: the lower channel first
    m_peak_db[5] = 20;
    m_peak_db[40] = 20;
    CHECK(spectrum_find_peak(0, &idx) && idx == 5, "tie, lower channel first: %d", idx);
    CHECK(spectrum_find_peak(1, &idx) && idx == 40, "tie, then the other: %d", idx);
}

static void test_occupancy(void)
{
    fresh(3);
    uint8_t db[3] = {0, 0, 0};
    set_live(db, 3);
    g_scan[0].busy = 0;
    g_scan[1].busy = 128;
    g_scan[2].busy = 255;

    // 10 seconds of sweeps every 10 ms
    for (uint32_t t = 1000; t < 11000; t += 10)
        spectrum_update(t);
    CHECK(spectrum_occupancy(0) == 0, "idle: %d", spectrum_occupancy(0));
    CHECK(spectrum_occupancy(1) == 50, "half busy: %d", spectrum_occupancy(1));
    CHECK(spectrum_occupancy(2) == 100, "always busy: %d", spectrum_occupancy(2));

    // After the warm-up it is a long average: a second of silence barely moves it
    for (uint32_t t = 11000; t < 50000; t += 10)
        spectrum_update(t);
    g_scan[2].busy = 0;
    for (uint32_t t = 50000; t < 51000; t += 10)
        spectrum_update(t);
    CHECK(spectrum_occupancy(2) >= 90, "one quiet second out of a busy half minute: %d",
          spectrum_occupancy(2));

    uint8_t top[3];
    uint8_t n = spectrum_top_busy(top, 3);
    CHECK(n == 2 && top[0] == 2 && top[1] == 1, "busiest by occupancy: n %d first %d", n,
          top[0]);
}

static void test_chan_col(void)
{
    for (int count = 1; count <= SCAN_MAX_CHANNELS; count++)
    {
        m_stub_count = (uint8_t)count;
        for (int idx = 0; idx < count; idx++)
        {
            int col = spectrum_chan_to_col((uint8_t)idx);
            uint8_t back = spectrum_col_to_chan(col);
            bool shown = false;
            for (int c = 0; c < DISP_W; c++)
                shown |= spectrum_col_to_chan(c) == idx;

            checks++;
            // A channel without a column of its own lands next to it
            if (col < 0 || col >= DISP_W || (shown && back != idx) ||
                (!shown && abs(back - idx) > 1))
            {
                failures++;
                printf("  FAIL chan_to_col count %d idx %d: col %d back %d\n", count, idx, col,
                       back);
                return;
            }
        }
    }
}

// Reference plot at any height, one pixel at a time
static void ref_plot(int top, int h, int marker_col)
{
    int bottom = top + h - 1;
    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = bottom - (db * h) / SPECTRUM_RANGE_DB;
        for (int x = 0; x < DISP_W; x += 8)
            gfx_pixel(x, y, true);
    }
    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = spectrum_col_to_chan(x);
        int bar = (spectrum_trace_db(idx) * h) / SPECTRUM_RANGE_DB;
        if (bar > h)
            bar = h;
        for (int y = bottom + 1 - bar; y <= bottom; y++)
            gfx_pixel(x, y, true);
        int cap = (m_max_db[idx] * h) / SPECTRUM_RANGE_DB;
        if (cap > h)
            cap = h;
        if (cap > 0 && (x & 1) == 0)
            gfx_pixel(x, bottom + 1 - cap, true);
        if (m_has_ref && (x & 3) == 0)
        {
            int r = (m_ref_db[idx] * h) / SPECTRUM_RANGE_DB;
            if (r > 0)
                gfx_pixel(x, bottom + 1 - r, true);
        }
    }
    if (marker_col >= 0 && marker_col < DISP_W)
    {
        for (int y = top; y <= bottom; y += 2)
            gfx_pixel(marker_col, y, !gfx_pixel_get(marker_col, y));
    }
}

static void test_tall_plot(void)
{
    static uint8_t want[DISP_BUF_SIZE];
    static const int tops[] = {8, 3, 16};
    static const int heights[] = {48, 57, 40};

    for (int state = 0; state < 200; state++)
    {
        m_stub_count = (uint8_t)(1 + rnd() % SCAN_MAX_CHANNELS);
        m_trace = (uint8_t)(rnd() % TRACE_COUNT);
        for (int i = 0; i < SCAN_MAX_CHANNELS; i++)
        {
            m_peak_db[i] = (uint8_t)(rnd() % 50);
            m_max_db[i] = (uint8_t)(rnd() % 50);
            m_ref_db[i] = (uint8_t)(rnd() % 41);
            m_min_db[i] = (uint8_t)(rnd() % 3 ? rnd() % 50 : MIN_UNSET);
            m_avg_q8[i] = (uint16_t)(rnd() % (50 << 8));
        }
        m_has_ref = rnd() & 1;
        int marker = (int)(rnd() % (DISP_W + 2)) - 1;

        for (int k = 0; k < 3; k++)
        {
            memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
            ref_plot(tops[k], heights[k], marker);
            memcpy(want, g_frame_buffer, sizeof(want));

            memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
            spectrum_draw_plot(tops[k], heights[k], marker, -1);
            checks++;
            if (memcmp(want, g_frame_buffer, sizeof(want)) != 0)
            {
                failures++;
                printf("  FAIL tall plot state %d top %d h %d\n", state, tops[k], heights[k]);
                return;
            }
        }
    }

    // The delta marker is a dashed line in its column, and nothing else
    m_stub_count = 84;
    memset(m_peak_db, 0, sizeof(m_peak_db));
    memset(m_max_db, 0, sizeof(m_max_db));
    m_trace = TRACE_PEAK;
    m_has_ref = false;
    memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
    spectrum_draw_plot(8, 48, -1, 30);
    int on = 0;
    for (int y = 8; y < 56; y++)
        on += gfx_pixel_get(30, y);
    CHECK(on == 24 && gfx_pixel_get(30, 8) && !gfx_pixel_get(30, 10), "delta dashes: %d on", on);
}

static void test_db_labels(void)
{
    static uint8_t want[DISP_BUF_SIZE];

    fresh(84);
    spectrum_set_cal(4);

    // Fixed floor -92 dBm plus 4: the lines at 10, 20, 30 dB read -78 -68 -58
    memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
    gfx_text_micro(1, 25 - 2, "-78");
    gfx_text_micro(1, 19 - 2, "-68");
    gfx_text_micro(1, 13 - 2, "-58");
    memcpy(want, g_frame_buffer, sizeof(want));

    memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
    spectrum_draw_db_labels(SPECTRUM_TOP, SPECTRUM_H);
    CHECK(memcmp(want, g_frame_buffer, sizeof(want)) == 0, "dBm labels on the split plot");

    // Drawn over a full bar they invert: the same digits come out dark
    memset(g_frame_buffer, 0xFF, sizeof(g_frame_buffer));
    spectrum_draw_db_labels(SPECTRUM_TOP, SPECTRUM_H);
    int dark = 0;
    for (int i = 0; i < DISP_BUF_SIZE; i++)
        dark += __builtin_popcount((uint8_t)~g_frame_buffer[i]);
    int ink = 0;
    for (int i = 0; i < DISP_BUF_SIZE; i++)
        ink += __builtin_popcount(want[i]);
    CHECK(dark == ink && ink > 0, "labels invert over data: %d dark, %d ink", dark, ink);
    spectrum_set_cal(0);
}

int main(void)
{
    printf("spectrum features\n");

    test_dwell();
    test_rbw();
    test_traces();
    test_levels();
    test_peaks();
    test_occupancy();
    test_chan_col();
    test_tall_plot();
    test_db_labels();

    printf("%s: %d of %d checks failed\n", failures ? "FAILED" : "PASSED", failures, checks);
    return failures ? 1 : 0;
}
