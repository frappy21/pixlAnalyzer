// Host side test of the scanner screen renderer: the byte at a time spectrum
// and waterfall must produce exactly the frame buffer the original per pixel
// code produced. spectrum.c is included so its state can be set directly; the
// scanner and the settings are stubbed, gfx and channels are the real code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/spectrum.c"
#include "font3x5.h"

uint8_t g_frame_buffer[DISP_BUF_SIZE];
settings_data_t g_settings;
chan_result_t g_scan[SCAN_MAX_CHANNELS];
uint8_t g_floor[SCAN_MAX_CHANNELS];

static uint8_t m_stub_count;
static uint16_t m_stub_lo = 2400;
static uint16_t m_stub_hi = 2483;

uint8_t scanner_count(void) { return m_stub_count; }
uint16_t scanner_span_start(void) { return m_stub_lo; }
uint16_t scanner_span_end(void) { return m_stub_hi; }
uint16_t scanner_mhz(uint8_t index) { return (uint16_t)(m_stub_lo + index); }

// ---------------------------------------------------------------------------
// The original implementation, kept verbatim apart from the names
// ---------------------------------------------------------------------------

static void ref_pixel(int x, int y, bool on)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H)
    {
        if (on)
            g_frame_buffer[x + (y / 8) * DISP_W] |= (1 << (y % 8));
        else
            g_frame_buffer[x + (y / 8) * DISP_W] &= ~(1 << (y % 8));
    }
}

static bool ref_pixel_get(int x, int y)
{
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H)
        return false;

    return (g_frame_buffer[x + (y / 8) * DISP_W] & (1 << (y % 8))) != 0;
}

static void ref_vline(int x, int y1, int y2)
{
    if (y1 > y2)
    {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    for (int y = y1; y <= y2; y++)
        ref_pixel(x, y, true);
}

static void ref_dither_pixel(int x, int y, uint8_t level)
{
    static const uint8_t bayer[2][2] = {{0, 2}, {3, 1}};

    if (level > bayer[x & 1][y & 1])
        ref_pixel(x, y, true);
}

static uint8_t ref_col_to_chan(int col)
{
    uint8_t count = scanner_count();
    if (count == 0)
        return 0;
    if (col < 0)
        col = 0;
    if (col >= DISP_W)
        col = DISP_W - 1;

    uint16_t idx = ((uint16_t)col * count) / DISP_W;
    return idx >= count ? count - 1 : (uint8_t)idx;
}

static uint8_t ref_history_level(uint16_t rows_back, int col)
{
    if (rows_back >= m_rows)
        return 0;

    uint16_t idx = (uint16_t)((m_head + HISTORY_ROWS - 1 - rows_back) % HISTORY_ROWS);
    uint8_t packed = m_history[idx][col / 4];
    return (packed >> ((col % 4) * 2)) & 3;
}

static void ref_spectrum_draw(int marker_col)
{
    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = SPECTRUM_TOP + SPECTRUM_H - 1 - (db * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        for (int x = 0; x < DISP_W; x += 8)
            ref_pixel(x, y, true);
    }

    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = ref_col_to_chan(x);

        int bar = (m_peak_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (bar > SPECTRUM_H)
            bar = SPECTRUM_H;
        if (bar > 0)
            ref_vline(x, SPECTRUM_TOP + SPECTRUM_H - bar, SPECTRUM_TOP + SPECTRUM_H - 1);

        int cap = (m_max_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (cap > SPECTRUM_H)
            cap = SPECTRUM_H;
        if (cap > 0 && (x & 1) == 0)
            ref_pixel(x, SPECTRUM_TOP + SPECTRUM_H - cap, true);

        if (m_has_ref && (x & 3) == 0)
        {
            int r = (m_ref_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
            if (r > 0)
                ref_pixel(x, SPECTRUM_TOP + SPECTRUM_H - r, true);
        }
    }

    if (marker_col >= 0 && marker_col < DISP_W)
    {
        for (int y = SPECTRUM_TOP; y < SPECTRUM_TOP + SPECTRUM_H; y += 2)
            ref_pixel(marker_col, y, !ref_pixel_get(marker_col, y));
    }
}

static void ref_spectrum_draw_waterfall(uint16_t scroll_back)
{
    bool dither = (g_settings.wf_mode == WF_DITHER);

    for (uint16_t r = 0; r < WATERFALL_ROWS; r++)
    {
        int y = WATERFALL_START + r;
        for (int x = 0; x < DISP_W; x++)
        {
            uint8_t level = ref_history_level(scroll_back + r, x);
            if (level == 0)
                continue;

            if (dither)
                ref_dither_pixel(x, y, level);
            else if (level >= 2)
                ref_pixel(x, y, true);
        }
    }
}

// ---------------------------------------------------------------------------

static int failures = 0;
static int checks = 0;

static uint32_t rnd(void)
{
    // xorshift32, fixed seed so a failure reproduces
    static uint32_t s = 0x2545F491u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint8_t rnd_db(void)
{
    // Mostly inside the plotted range, sometimes zero or far above it
    switch (rnd() % 8)
    {
    case 0:
        return 0;
    case 1:
        return (uint8_t)rnd();
    default:
        return (uint8_t)(rnd() % (SPECTRUM_RANGE_DB + 1));
    }
}

static void random_state(void)
{
    m_stub_count = (uint8_t)(rnd() % 6 == 0 ? rnd() % 4 : 1 + rnd() % SCAN_MAX_CHANNELS);
    m_stub_lo = (uint16_t)(2360 + rnd() % 60);
    m_stub_hi = (uint16_t)(m_stub_lo + 1 + rnd() % 140);

    for (int i = 0; i < SCAN_MAX_CHANNELS; i++)
    {
        m_peak_db[i] = rnd_db();
        m_max_db[i] = rnd_db();
        m_ref_db[i] = rnd_db();
    }
    m_has_ref = rnd() & 1;

    // Sparse, dense or saturated history
    uint32_t density = rnd() % 4;
    for (int r = 0; r < HISTORY_ROWS; r++)
    {
        for (int k = 0; k < ROW_BYTES; k++)
        {
            uint8_t b = (uint8_t)rnd();
            if (density == 0)
                b &= (uint8_t)rnd() & (uint8_t)rnd();
            else if (density == 3)
                b = 0xFF;
            m_history[r][k] = b;
        }
    }
    m_head = (uint16_t)(rnd() % HISTORY_ROWS);
    m_rows = (uint16_t)(rnd() % 3 == 0 ? HISTORY_ROWS : rnd() % (HISTORY_ROWS + 1));
}

static void random_background(uint8_t *bg)
{
    uint32_t kind = rnd() % 3;
    for (int i = 0; i < DISP_BUF_SIZE; i++)
        bg[i] = kind == 0 ? 0 : kind == 1 ? 0xFF : (uint8_t)rnd();
}

static void compare(const char *what, const uint8_t *want, const uint8_t *got, int state)
{
    checks++;
    if (memcmp(want, got, DISP_BUF_SIZE) == 0)
        return;

    failures++;
    for (int i = 0; i < DISP_BUF_SIZE; i++)
    {
        if (want[i] != got[i])
        {
            printf("  FAIL %s state %d: x %d page %d want %02X got %02X\n", what, state,
                   i % DISP_W, i / DISP_W, want[i], got[i]);
            break;
        }
    }
}

static void test_col_to_chan(void)
{
    // Every count, and the span changing under the same count
    for (int pass = 0; pass < 2; pass++)
    {
        for (int count = 0; count <= SCAN_MAX_CHANNELS; count++)
        {
            m_stub_count = (uint8_t)count;
            m_stub_hi = (uint16_t)(2483 + pass);
            for (int col = -3; col < DISP_W + 3; col++)
            {
                checks++;
                if (spectrum_col_to_chan(col) != ref_col_to_chan(col))
                {
                    printf("  FAIL col_to_chan count %d col %d\n", count, col);
                    failures++;
                }
            }
        }
    }
}

// Old and new ruler must agree wherever no label is dropped
static void test_ruler_sparse(void)
{
    static const uint8_t plans[] = {PLAN_NONE, PLAN_BLE};
    uint8_t want[DISP_BUF_SIZE];

    for (unsigned p = 0; p < sizeof(plans); p++)
    {
        for (int marker = -1; marker <= DISP_W; marker += 43)
        {
            m_stub_lo = 2400;
            m_stub_hi = 2483;

            // The rows the ruler owns: its line, the brackets and the labels.
            // Without a plan it stops after the line, marker included.
            memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
            for (int x = 0; x < DISP_W; x++)
                ref_pixel(x, RULER_Y, true);
            if (plans[p] != PLAN_NONE)
            {
                for (uint8_t i = 0; i < channels_plan_count(plans[p]); i++)
                {
                    chan_mark_t mark;
                    channels_plan_get(plans[p], i, &mark);
                    int cx = ((mark.center_mhz - 2400) * (DISP_W - 1)) / 83;
                    char label[3] = {(char)('0' + mark.number / 10),
                                     (char)('0' + mark.number % 10), 0};
                    gfx_text_micro(cx - gfx_text_micro_width(label) / 2, RULER_Y + 2, label);
                }
                if (marker >= 0 && marker < DISP_W)
                    ref_pixel(marker, RULER_Y + 1, true);
            }
            memcpy(want, g_frame_buffer, sizeof(want));

            memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
            spectrum_draw_ruler(plans[p], marker);
            compare("ruler", want, g_frame_buffer, (int)p);
        }
    }
}

// The WiFi plan on the ISM span used to print "10111213". Now every label is
// set apart by more blank columns than the one between two digits of a label,
// so the text row splits into words of exactly one or two digits.
static void test_ruler_gap(void)
{
    m_stub_lo = 2400;
    m_stub_hi = 2483;
    memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
    spectrum_draw_ruler(PLAN_WIFI, -1);

    int word = 0, blank = 0, words = 0, bad = 0;
    for (int x = 0; x <= DISP_W; x++)
    {
        bool ink = false;
        for (int y = RULER_Y + 2; x < DISP_W && y < RULER_Y + 2 + MICRO_HEIGHT; y++)
            ink |= gfx_pixel_get(x, y);

        if (ink)
        {
            // A single blank column is the gap inside a label
            if (word && blank == 1)
                word++;
            word += 1;
            blank = 0;
            continue;
        }
        blank++;
        if (word && (blank >= 2 || x == DISP_W))
        {
            if (word != MICRO_WIDTH && word != 2 * MICRO_ADVANCE - 1)
                bad++;
            words++;
            word = 0;
        }
    }

    checks++;
    if (bad || words < 10)
    {
        printf("  FAIL ruler gap: %d labels, %d run together\n", words, bad);
        failures++;
    }
    else
    {
        printf("  ok   wifi ruler: %d labels, none touching\n", words);
    }
}

int main(void)
{
    static uint8_t bg[DISP_BUF_SIZE];
    static uint8_t want[DISP_BUF_SIZE];
    static const int markers[] = {-1, 0, 1, 63, 126, 127, 128};

    printf("spectrum render\n");

    test_col_to_chan();
    test_ruler_sparse();
    test_ruler_gap();

    for (int state = 0; state < 400; state++)
    {
        random_state();
        random_background(bg);

        for (unsigned m = 0; m < sizeof(markers) / sizeof(markers[0]); m++)
        {
            int marker = m == 0 ? (int)(rnd() % DISP_W) : markers[m];

            memcpy(g_frame_buffer, bg, sizeof(bg));
            ref_spectrum_draw(marker);
            memcpy(want, g_frame_buffer, sizeof(want));

            memcpy(g_frame_buffer, bg, sizeof(bg));
            spectrum_draw(marker);
            compare("spectrum", want, g_frame_buffer, state);
        }

        uint16_t scrolls[] = {0,
                              1,
                              (uint16_t)(rnd() % (m_rows + 1)),
                              (uint16_t)(m_rows > WATERFALL_ROWS ? m_rows - WATERFALL_ROWS : 0),
                              (uint16_t)(m_rows ? m_rows - 1 : 0),
                              m_rows,
                              (uint16_t)rnd(),
                              65530};

        for (int mode = 0; mode < WF_MODE_COUNT; mode++)
        {
            g_settings.wf_mode = (uint8_t)mode;
            for (unsigned k = 0; k < sizeof(scrolls) / sizeof(scrolls[0]); k++)
            {
                memcpy(g_frame_buffer, bg, sizeof(bg));
                ref_spectrum_draw_waterfall(scrolls[k]);
                memcpy(want, g_frame_buffer, sizeof(want));

                memcpy(g_frame_buffer, bg, sizeof(bg));
                spectrum_draw_waterfall(scrolls[k]);
                compare(mode ? "waterfall dither" : "waterfall threshold", want,
                        g_frame_buffer, state);
            }
        }
    }

    printf("%s: %d of %d frame comparisons failed\n", failures ? "FAILED" : "PASSED", failures,
           checks);
    return failures ? 1 : 0;
}
