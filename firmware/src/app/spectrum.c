#include <string.h>

#include "app_config.h"
#include "board_config.h"
#include "channels.h"
#include "display.h"
#include "gfx.h"
#include "scanner.h"
#include "settings.h"
#include "spectrum.h"

#define ROW_BYTES (DISP_W / 4) // two bits per column

// The spectrum plot is drawn a column at a time into one 32 bit mask, bit 0 is
// the top row of the first frame buffer page it touches
#define SPEC_PAGE0 (SPECTRUM_TOP / 8)
#define SPEC_BOTTOM (SPECTRUM_TOP + SPECTRUM_H - 1)
#define SPEC_PAGES (SPEC_BOTTOM / 8 - SPEC_PAGE0 + 1)
#define SPEC_BIT(y) ((y) - SPEC_PAGE0 * 8)

// Blank columns kept between ruler labels. Digits inside a label are already
// one column apart, so one column between labels would read as one number.
#define LABEL_GAP 2

_Static_assert(SPEC_PAGES <= 4, "spectrum plot must fit in a 32 bit column mask");
_Static_assert(SPECTRUM_H >= 1 && SPEC_BOTTOM < DISP_H, "spectrum plot off screen");
_Static_assert(WATERFALL_START >= 0 && WATERFALL_START + WATERFALL_ROWS <= DISP_H,
               "waterfall off screen");

static uint8_t m_live_db[SCAN_MAX_CHANNELS];
static uint8_t m_peak_db[SCAN_MAX_CHANNELS];
static uint8_t m_max_db[SCAN_MAX_CHANNELS];
static uint8_t m_ref_db[SCAN_MAX_CHANNELS];
static bool m_has_ref;

// Waterfall history ring, newest row at m_head - 1
static uint8_t m_history[HISTORY_ROWS][ROW_BYTES];
static uint16_t m_head;
static uint16_t m_rows;

// Channel under each column, rebuilt when the scanner configuration changes
static uint8_t m_col_chan[DISP_W];
static uint8_t m_lut_count;
static uint16_t m_lut_lo;
static uint16_t m_lut_hi;

static uint8_t m_accum[DISP_W]; // levels accumulated between waterfall rows
static uint8_t m_accum_count;
static uint32_t m_sweeps;
static uint32_t m_last_decay_ms;

void spectrum_reset(void)
{
    memset(m_live_db, 0, sizeof(m_live_db));
    memset(m_peak_db, 0, sizeof(m_peak_db));
    memset(m_max_db, 0, sizeof(m_max_db));
    memset(m_accum, 0, sizeof(m_accum));
    m_accum_count = 0;
    m_head = 0;
    m_rows = 0;
    m_sweeps = 0;
    m_last_decay_ms = 0;
    m_has_ref = false;
}

void spectrum_clear_max(void)
{
    memset(m_max_db, 0, sizeof(m_max_db));
}

void spectrum_snapshot_ref(void)
{
    memcpy(m_ref_db, m_peak_db, sizeof(m_ref_db));
    m_has_ref = true;
}

void spectrum_clear_ref(void) { m_has_ref = false; }
bool spectrum_has_ref(void) { return m_has_ref; }

uint8_t spectrum_db(uint8_t chan_index) { return m_live_db[chan_index]; }
uint8_t spectrum_max_db(uint8_t chan_index) { return m_max_db[chan_index]; }
uint16_t spectrum_history_rows(void) { return m_rows; }
uint32_t spectrum_sweeps(void) { return m_sweeps; }

// The scanner does not announce configuration changes, so compare what the
// table was built for on every use. Three getters are far cheaper than a
// division per column.
static const uint8_t *col_chan_table(void)
{
    uint8_t count = scanner_count();
    uint16_t lo = scanner_span_start();
    uint16_t hi = scanner_span_end();

    if (count != m_lut_count || lo != m_lut_lo || hi != m_lut_hi)
    {
        for (int col = 0; col < DISP_W; col++)
            m_col_chan[col] = (uint8_t)(((uint16_t)col * count) / DISP_W);
        m_lut_count = count;
        m_lut_lo = lo;
        m_lut_hi = hi;
    }
    return m_col_chan;
}

uint8_t spectrum_col_to_chan(int col)
{
    if (col < 0)
        col = 0;
    if (col >= DISP_W)
        col = DISP_W - 1;

    return col_chan_table()[col];
}

int spectrum_chan_to_col(uint8_t chan_index)
{
    uint8_t count = scanner_count();
    if (count == 0)
        return 0;
    return ((int)chan_index * DISP_W) / count + (DISP_W / count) / 2;
}

// dB the channel sits above its own tracked noise floor
static uint8_t db_above_floor(uint8_t idx)
{
    uint8_t peak = g_scan[idx].peak;
    uint8_t floor_v = g_settings.auto_floor ? g_floor[idx] : NOISE_FLOOR_INIT;

    if (peak >= floor_v || peak == RSSI_INVALID)
        return 0;

    uint8_t db = floor_v - peak;
    return db > SPECTRUM_RANGE_DB ? SPECTRUM_RANGE_DB : db;
}

static uint8_t level_of(uint8_t db, uint8_t busy)
{
    // Four levels for the dithered waterfall: how strong, not just present
    if (db < SIGNAL_MARGIN_DB && busy < 8)
        return 0;
    if (db < 12)
        return 1;
    if (db < 22)
        return 2;
    return 3;
}

static void history_push(void)
{
    uint8_t *row = m_history[m_head];
    memset(row, 0, ROW_BYTES);

    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t level = m_accum[x] & 3;
        row[x / 4] |= (uint8_t)(level << ((x % 4) * 2));
    }

    m_head = (uint16_t)((m_head + 1) % HISTORY_ROWS);
    if (m_rows < HISTORY_ROWS)
        m_rows++;

    memset(m_accum, 0, sizeof(m_accum));
    m_accum_count = 0;
}

bool spectrum_update(uint32_t now_ms)
{
    uint8_t count = scanner_count();
    m_sweeps++;

    // Peak hold decays by time, not by frame, so the display behaves the same
    // whether the sweep runs at 20Hz or 150Hz
    uint32_t elapsed = now_ms - m_last_decay_ms;
    uint8_t decay = 0;
    if (m_last_decay_ms && elapsed)
    {
        decay = (uint8_t)((elapsed * PEAK_DECAY_DB_PER_S) / 1000u);
        if (decay)
            m_last_decay_ms = now_ms;
    }
    else if (!m_last_decay_ms)
    {
        m_last_decay_ms = now_ms;
    }

    // One update per channel, never per column: mapping 141 channels onto 128
    // columns used to decay some channels twice as fast as others
    for (uint8_t i = 0; i < count; i++)
    {
        uint8_t db = db_above_floor(i);
        m_live_db[i] = db;

        if (db >= m_peak_db[i])
            m_peak_db[i] = db;
        else if (m_peak_db[i] > decay)
            m_peak_db[i] -= decay;
        else
            m_peak_db[i] = 0;

        if (db > m_max_db[i])
            m_max_db[i] = db;
    }

    // Accumulate the waterfall row: one row is the maximum over wf_decim sweeps,
    // which is what makes sparse traffic visible at all
    const uint8_t *col_chan = col_chan_table();
    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = col_chan[x];
        uint8_t level = level_of(m_live_db[idx], g_scan[idx].busy);
        if (level > m_accum[x])
            m_accum[x] = level;
    }

    uint8_t decim = g_settings.wf_decim ? g_settings.wf_decim : 1;
    if (++m_accum_count >= decim)
    {
        history_push();
        return true;
    }
    return false;
}

// Rows first to last of the plot as a column mask
static uint32_t spec_rows(int first, int last)
{
    return (0xFFFFFFFFu >> (31 - (last - first))) << SPEC_BIT(first);
}

void spectrum_draw(int marker_col)
{
    // dB grid, a dot every 8 columns
    uint32_t grid = 0;
    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = SPEC_BOTTOM - (db * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        grid |= 1u << SPEC_BIT(y);
    }

    const uint8_t *col_chan = col_chan_table();
    uint8_t *fb = &g_frame_buffer[SPEC_PAGE0 * DISP_W];

    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = col_chan[x];
        uint32_t col = (x & 7) == 0 ? grid : 0;

        int bar = (m_peak_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (bar > SPECTRUM_H)
            bar = SPECTRUM_H;
        if (bar > 0)
            col |= spec_rows(SPEC_BOTTOM + 1 - bar, SPEC_BOTTOM);

        // Max hold as a dotted cap above the live bar
        int cap = (m_max_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (cap > SPECTRUM_H)
            cap = SPECTRUM_H;
        if (cap > 0 && (x & 1) == 0)
            col |= 1u << SPEC_BIT(SPEC_BOTTOM + 1 - cap);

        // Reference trace as a sparse dashed line. It is not clamped, so a
        // value above the range lands above the plot like it always did.
        if (m_has_ref && (x & 3) == 0)
        {
            int r = (m_ref_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
            if (r > SPECTRUM_H)
                gfx_pixel(x, SPEC_BOTTOM + 1 - r, true);
            else if (r > 0)
                col |= 1u << SPEC_BIT(SPEC_BOTTOM + 1 - r);
        }

        for (int p = 0; p < SPEC_PAGES; p++)
            fb[p * DISP_W + x] |= (uint8_t)(col >> (p * 8));
    }

    // Marker as a dotted line that inverts whatever is under it
    if (marker_col >= 0 && marker_col < DISP_W)
    {
        uint32_t dots = 0;
        for (int y = SPECTRUM_TOP; y <= SPEC_BOTTOM; y += 2)
            dots |= 1u << SPEC_BIT(y);
        for (int p = 0; p < SPEC_PAGES; p++)
            fb[p * DISP_W + marker_col] ^= (uint8_t)(dots >> (p * 8));
    }
}

// Spreads the level flags of one history byte (bits 0, 2, 4, 6 for its four
// columns) to bit 0 of four bytes, column 4k + c ending up in byte c. The
// partial products never meet at bits 0, 8, 16 or 24, so no carry spoils them.
static inline uint32_t spread4(uint32_t flags)
{
    return (flags * 0x41041u) & 0x01010101u;
}

void spectrum_draw_waterfall(uint16_t scroll_back)
{
    bool dither = (g_settings.wf_mode == WF_DITHER);

    // Which level each pixel needs, as masks over the column fields of a
    // history byte (even columns at bits 0 and 4, odd at 2 and 6), per row
    // parity. The dither is the ordered 2x2 Bayer matrix {{0, 2}, {3, 1}}
    // indexed [x & 1][y & 1]: a pixel is on when its level exceeds the entry.
    // Threshold mode draws levels 2 and 3 solid.
    static const uint8_t need[2][2][3] = {
        // level >= 1, >= 2, >= 3
        {{0x00, 0x55, 0x00}, {0x00, 0x55, 0x00}}, // threshold
        {{0x11, 0x00, 0x00}, {0x00, 0x44, 0x11}}, // dither, y even / y odd
    };

    int y_end = WATERFALL_START + WATERFALL_ROWS;
    for (int page = WATERFALL_START / 8; page * 8 < y_end; page++)
    {
        // Four columns per word, one byte each, bit (y % 8) per row
        uint32_t acc[ROW_BYTES] = {0};
        bool any = false;

        int y0 = page * 8 < WATERFALL_START ? WATERFALL_START : page * 8;
        int y1 = page * 8 + 8 < y_end ? page * 8 + 8 : y_end;
        for (int y = y0; y < y1; y++)
        {
            uint16_t rows_back = (uint16_t)(scroll_back + (y - WATERFALL_START));
            if (rows_back >= m_rows)
                continue;

            uint16_t ring = (uint16_t)(m_head + HISTORY_ROWS - 1 - rows_back);
            if (ring >= HISTORY_ROWS)
                ring -= HISTORY_ROWS;
            const uint8_t *row = m_history[ring];
            const uint8_t *m = need[dither][y & 1];
            int shift = y & 7;

            for (int k = 0; k < ROW_BYTES; k++)
            {
                uint8_t b = row[k];
                if (!b)
                    continue;

                uint8_t ge1 = (b | (b >> 1)) & 0x55;
                uint8_t ge2 = (b >> 1) & 0x55;
                uint8_t ge3 = b & (b >> 1) & 0x55;
                uint8_t on = (ge1 & m[0]) | (ge2 & m[1]) | (ge3 & m[2]);
                if (on)
                {
                    acc[k] |= spread4(on) << shift;
                    any = true;
                }
            }
        }

        if (!any)
            continue;

        uint8_t *fb = &g_frame_buffer[page * DISP_W];
        for (int k = 0; k < ROW_BYTES; k++)
        {
            uint32_t w = acc[k];
            fb[4 * k + 0] |= (uint8_t)w;
            fb[4 * k + 1] |= (uint8_t)(w >> 8);
            fb[4 * k + 2] |= (uint8_t)(w >> 16);
            fb[4 * k + 3] |= (uint8_t)(w >> 24);
        }
    }
}

void spectrum_draw_ruler(uint8_t plan, int marker_col)
{
    // A dedicated row so labels never sit on top of the data
    uint8_t *fb = &g_frame_buffer[(RULER_Y / 8) * DISP_W];
    for (int x = 0; x < DISP_W; x++)
        fb[x] |= (uint8_t)(1u << (RULER_Y % 8));

    if (plan == PLAN_NONE)
        return;

    uint16_t lo = scanner_span_start();
    uint16_t hi = scanner_span_end();
    if (hi <= lo)
        return;

    // Right edge of the last label drawn
    int last_end = -1 - LABEL_GAP;

    for (uint8_t i = 0; i < channels_plan_count(plan); i++)
    {
        chan_mark_t mark;
        if (!channels_plan_get(plan, i, &mark))
            continue;
        if (mark.center_mhz < lo || mark.center_mhz > hi)
            continue;

        int cx = ((mark.center_mhz - lo) * (DISP_W - 1)) / (hi - lo);

        // Width bracket for wide plans, a tick for narrow ones
        if (mark.half_width > 2)
        {
            int x0 = ((mark.center_mhz - mark.half_width - lo) * (DISP_W - 1)) / (hi - lo);
            int x1 = ((mark.center_mhz + mark.half_width - lo) * (DISP_W - 1)) / (hi - lo);
            if (x0 < 0)
                x0 = 0;
            if (x1 > DISP_W - 1)
                x1 = DISP_W - 1;
            gfx_pixel(x0, RULER_Y + 1, true);
            gfx_pixel(x1, RULER_Y + 1, true);
        }

        char label[4];
        char *p = label;
        if (mark.number >= 10)
            *p++ = (char)('0' + mark.number / 10);
        *p++ = (char)('0' + mark.number % 10);
        *p = '\0';

        int w = gfx_text_micro_width(label);
        int lx = cx - w / 2;
        if (lx < 0)
            lx = 0;
        if (lx + w >= DISP_W)
            lx = DISP_W - 1 - w;

        // Dense plans (WiFi 10..13) would run their labels together: drop a
        // label that would touch the previous one, its bracket still shows
        if (lx <= last_end + LABEL_GAP)
            continue;
        gfx_text_micro(lx, RULER_Y + 2, label);
        last_end = lx + w - 1;
    }

    if (marker_col >= 0 && marker_col < DISP_W)
        gfx_pixel(marker_col, RULER_Y + 1, true);
}

uint8_t spectrum_strongest(void)
{
    uint8_t best = 0;
    uint8_t best_db = 0;
    for (uint8_t i = 0; i < scanner_count(); i++)
    {
        if (m_live_db[i] > best_db)
        {
            best_db = m_live_db[i];
            best = i;
        }
    }
    return best;
}

uint8_t spectrum_top_busy(uint8_t *idx, uint8_t n)
{
    uint8_t count = scanner_count();
    uint8_t written = 0;

    for (uint8_t slot = 0; slot < n; slot++)
    {
        int best = -1;
        uint16_t best_score = 0;

        for (uint8_t i = 0; i < count; i++)
        {
            bool taken = false;
            for (uint8_t k = 0; k < written; k++)
            {
                if (idx[k] == i)
                    taken = true;
            }
            if (taken)
                continue;

            uint16_t score = (uint16_t)g_scan[i].busy + m_max_db[i];
            if (score > best_score)
            {
                best_score = score;
                best = i;
            }
        }

        if (best < 0 || best_score == 0)
            break;

        idx[written++] = (uint8_t)best;
    }
    return written;
}
