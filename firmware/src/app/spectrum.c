#include <string.h>

#include "app_config.h"
#include "board_config.h"
#include "channels.h"
#include "display.h"
#include "font3x5.h"
#include "gfx.h"
#include "scanner.h"
#include "settings.h"
#include "spectrum.h"

#define ROW_BYTES (DISP_W / 4) // two bits per column

// The spectrum plot is drawn a column at a time into one 64 bit mask, bit 0 is
// the top row of the first frame buffer page it touches
#define PLOT_BIT(y, page0) ((y) - (page0) * 8)

// Blank columns kept between ruler labels. Digits inside a label are already
// one column apart, so one column between labels would read as one number.
#define LABEL_GAP 2

// Average trace: every sweep moves it 1/2^AVG_SHIFT of the way to the live
// value. At ~118 sweeps per second that is a time constant of about 0.14s,
// enough to calm the noise without hiding a burst train.
#define AVG_SHIFT 4

// Min hold before its first sweep
#define MIN_UNSET 0xFF

// Long window occupancy: the per visit busy share is averaged over the sweeps
// of one OCC_TICK_MS tick, then folded into an exponential average over
// OCC_WINDOW_TICKS ticks, about 32s. Until that many ticks have passed it is
// a plain running mean, so the first seconds are not biased towards zero.
#define OCC_TICK_MS 250
#define OCC_WINDOW_TICKS 128

// Peak search: a peak must be this far above the noise floor, and this far
// above the lowest point between it and any higher peak (its prominence)
#define PEAK_MIN_DB SIGNAL_MARGIN_DB
#define PEAK_PROMINENCE_DB 6

_Static_assert(SPECTRUM_H >= 1 && SPECTRUM_TOP + SPECTRUM_H - 1 < DISP_H, "spectrum plot off screen");
_Static_assert(WATERFALL_START >= 0 && WATERFALL_START + WATERFALL_ROWS <= DISP_H,
               "waterfall off screen");

static uint8_t m_live_db[SCAN_MAX_CHANNELS];
static uint8_t m_peak_db[SCAN_MAX_CHANNELS];
static uint8_t m_max_db[SCAN_MAX_CHANNELS];
static uint8_t m_min_db[SCAN_MAX_CHANNELS];
static uint16_t m_avg_q8[SCAN_MAX_CHANNELS]; // dB, 8 fractional bits
static uint8_t m_ref_db[SCAN_MAX_CHANNELS];
static bool m_has_ref;

// Long window occupancy, 0..255 with 8 fractional bits, and the busy shares
// summed over the sweeps of the current tick
static uint16_t m_occ_q8[SCAN_MAX_CHANNELS];
static uint16_t m_occ_acc[SCAN_MAX_CHANNELS];
static uint8_t m_occ_sweeps;
static uint8_t m_occ_ticks;
static uint32_t m_occ_last_ms;

// Session options
static uint8_t m_trace = TRACE_PEAK;
static uint8_t m_rbw = 1;
static int8_t m_cal_db;

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
    memset(m_min_db, MIN_UNSET, sizeof(m_min_db));
    memset(m_avg_q8, 0, sizeof(m_avg_q8));
    memset(m_occ_q8, 0, sizeof(m_occ_q8));
    memset(m_occ_acc, 0, sizeof(m_occ_acc));
    m_occ_sweeps = 0;
    m_occ_ticks = 0;
    m_occ_last_ms = 0;
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
    memset(m_min_db, MIN_UNSET, sizeof(m_min_db));
}

uint8_t spectrum_trace_db(uint8_t chan_index)
{
    switch (m_trace)
    {
    case TRACE_AVG:
        return (uint8_t)((m_avg_q8[chan_index] + 128u) >> 8);
    case TRACE_MIN:
        return m_min_db[chan_index] == MIN_UNSET ? 0 : m_min_db[chan_index];
    case TRACE_PEAK:
    default:
        return m_peak_db[chan_index];
    }
}

void spectrum_snapshot_ref(void)
{
    // What is on screen, whichever trace that is
    for (int i = 0; i < SCAN_MAX_CHANNELS; i++)
        m_ref_db[i] = spectrum_trace_db((uint8_t)i);
    m_has_ref = true;
}

void spectrum_clear_ref(void) { m_has_ref = false; }
bool spectrum_has_ref(void) { return m_has_ref; }

void spectrum_set_trace(uint8_t mode)
{
    m_trace = mode < TRACE_COUNT ? mode : TRACE_PEAK;
    if (m_trace == TRACE_MIN)
        memset(m_min_db, MIN_UNSET, sizeof(m_min_db));
}

uint8_t spectrum_trace(void) { return m_trace; }

const char *spectrum_trace_name(uint8_t mode)
{
    switch (mode)
    {
    case TRACE_AVG:
        return "AVG";
    case TRACE_MIN:
        return "MIN";
    default:
        return "PEAK";
    }
}

void spectrum_set_rbw(uint8_t mhz) { m_rbw = mhz == 2 ? 2 : 1; }
uint8_t spectrum_rbw(void) { return m_rbw; }

void spectrum_set_cal(int8_t db)
{
    if (db < SPECTRUM_CAL_MIN_DB)
        db = SPECTRUM_CAL_MIN_DB;
    if (db > SPECTRUM_CAL_MAX_DB)
        db = SPECTRUM_CAL_MAX_DB;
    m_cal_db = db;
}

int8_t spectrum_cal(void) { return m_cal_db; }

int spectrum_dbm(uint8_t rssi) { return -(int)rssi + m_cal_db; }

uint8_t spectrum_db(uint8_t chan_index) { return m_live_db[chan_index]; }
uint8_t spectrum_max_db(uint8_t chan_index) { return m_max_db[chan_index]; }
uint16_t spectrum_history_rows(void) { return m_rows; }
uint32_t spectrum_sweeps(void) { return m_sweeps; }

static uint8_t floor_of(uint8_t idx)
{
    return g_settings.auto_floor ? g_floor[idx] : NOISE_FLOOR_INIT;
}

uint8_t spectrum_floor_rssi(void)
{
    uint8_t count = scanner_count();
    if (!g_settings.auto_floor || count == 0)
        return NOISE_FLOOR_INIT;

    uint32_t sum = 0;
    for (uint8_t i = 0; i < count; i++)
        sum += g_floor[i];
    return (uint8_t)((sum + count / 2) / count);
}

bool spectrum_level_dbm(uint8_t chan_index, int *dbm)
{
    if (chan_index >= scanner_count())
        return false;

    // Peak mode keeps what the readout always was: the strongest sample of
    // the last visit. The other traces are dB over the floor, so they are
    // turned back into a level against that same floor.
    if (m_trace == TRACE_PEAK)
    {
        uint8_t peak = g_scan[chan_index].peak;
        if (peak == RSSI_INVALID)
            return false;
        *dbm = spectrum_dbm(peak);
        return true;
    }

    *dbm = spectrum_dbm(floor_of(chan_index)) + spectrum_trace_db(chan_index);
    return true;
}

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
    // The exact inverse of spectrum_col_to_chan: the middle of the columns
    // showing the channel. With more channels than columns (the extended
    // band) some channels have no column, those get the next one.
    const uint8_t *col_chan = col_chan_table();
    int first = -1;
    int last = DISP_W - 1;
    for (int col = 0; col < DISP_W; col++)
    {
        if (first < 0 && col_chan[col] >= chan_index)
            first = col;
        if (first >= 0 && col_chan[col] != col_chan[first])
        {
            last = col - 1;
            break;
        }
    }
    if (first < 0)
        return DISP_W - 1;
    return (first + last) / 2;
}

// dB the channel sits above its own tracked noise floor
static uint8_t db_above_floor(uint8_t idx)
{
    uint8_t peak = g_scan[idx].peak;
    uint8_t floor_v = floor_of(idx);

    if (peak >= floor_v || peak == RSSI_INVALID)
        return 0;

    uint8_t db = floor_v - peak;
    return db > SPECTRUM_RANGE_DB ? SPECTRUM_RANGE_DB : db;
}

// 2 MHz resolution bandwidth. The radio only tunes whole MHz and its receive
// filter stays what it is, so there is no real 2 MHz filter here: two adjacent
// 1 MHz readings (an even MHz and the odd one above it) are combined into the
// power mean over both, and both columns show the result. The two readings
// are taken at different moments of the sweep, so a burst seen on one half
// counts as if it had lasted over both. What it does give is what a wider
// RBW gives on a real analyzer: a smoother trace, wideband signals at their
// level and narrowband ones up to 3 dB lower against the doubled noise.
//
// Power mean of a and b in dB, from the larger one: 10log10((1 + 10^(-d/10)) / 2)
// with d = |a - b|, rounded, is the loss below
static const uint8_t rbw2_loss[] = {0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 3};

static uint8_t rbw2_combine(uint8_t a, uint8_t b)
{
    uint8_t hi = a > b ? a : b;
    uint8_t d = a > b ? a - b : b - a;
    uint8_t loss = d < sizeof(rbw2_loss) ? rbw2_loss[d] : 3;
    return hi > loss ? hi - loss : 0;
}

static void rbw2_apply(uint8_t count)
{
    for (uint8_t i = 0; i + 1 < count; i++)
    {
        uint16_t mhz = scanner_mhz(i);
        if ((mhz & 1) || scanner_mhz(i + 1) != mhz + 1)
            continue;

        uint8_t v = rbw2_combine(m_live_db[i], m_live_db[i + 1]);
        m_live_db[i] = v;
        m_live_db[i + 1] = v;
        i++;
    }
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

static void occupancy_update(uint8_t count, uint32_t now_ms)
{
    for (uint8_t i = 0; i < count; i++)
        m_occ_acc[i] += g_scan[i].busy;

    // 255 sweeps of 255 still fit the 16 bit sums
    if (++m_occ_sweeps < 255 && now_ms - m_occ_last_ms < OCC_TICK_MS)
        return;

    if (m_occ_ticks < OCC_WINDOW_TICKS)
        m_occ_ticks++;

    for (uint8_t i = 0; i < count; i++)
    {
        int32_t target = ((int32_t)m_occ_acc[i] << 8) / m_occ_sweeps;
        int32_t occ = m_occ_q8[i];
        occ += (target - occ) / m_occ_ticks;
        m_occ_q8[i] = (uint16_t)occ;
        m_occ_acc[i] = 0;
    }
    m_occ_sweeps = 0;
    m_occ_last_ms = now_ms;
}

uint8_t spectrum_occupancy(uint8_t chan_index)
{
    return (uint8_t)(((uint32_t)m_occ_q8[chan_index] * 100u + (255u << 7)) / (255u << 8));
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

    for (uint8_t i = 0; i < count; i++)
        m_live_db[i] = db_above_floor(i);

    if (m_rbw == 2)
        rbw2_apply(count);

    // One update per channel, never per column: mapping 141 channels onto 128
    // columns used to decay some channels twice as fast as others
    for (uint8_t i = 0; i < count; i++)
    {
        uint8_t db = m_live_db[i];

        if (db >= m_peak_db[i])
            m_peak_db[i] = db;
        else if (m_peak_db[i] > decay)
            m_peak_db[i] -= decay;
        else
            m_peak_db[i] = 0;

        if (db > m_max_db[i])
            m_max_db[i] = db;
        if (db < m_min_db[i])
            m_min_db[i] = db;

        int32_t avg = m_avg_q8[i];
        avg += (((int32_t)db << 8) - avg) / (1 << AVG_SHIFT);
        m_avg_q8[i] = (uint16_t)avg;
    }

    occupancy_update(count, now_ms);

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
static uint64_t plot_rows(int first, int last, int page0)
{
    return (~0ull >> (63 - (last - first))) << PLOT_BIT(first, page0);
}

void spectrum_draw_plot(int top, int h, int marker_col, int delta_col)
{
    int bottom = top + h - 1;
    if (h < 1 || top < 0 || bottom >= DISP_H || (top & 7) + h > 64)
        return;

    int page0 = top / 8;
    int pages = bottom / 8 - page0 + 1;

    // dB grid, a dot every 8 columns
    uint64_t grid = 0;
    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = bottom - (db * h) / SPECTRUM_RANGE_DB;
        grid |= 1ull << PLOT_BIT(y, page0);
    }

    const uint8_t *col_chan = col_chan_table();
    uint8_t *fb = &g_frame_buffer[page0 * DISP_W];

    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = col_chan[x];
        uint64_t col = (x & 7) == 0 ? grid : 0;

        int bar = (spectrum_trace_db(idx) * h) / SPECTRUM_RANGE_DB;
        if (bar > h)
            bar = h;
        if (bar > 0)
            col |= plot_rows(bottom + 1 - bar, bottom, page0);

        // Max hold as a dotted cap above the live bar
        int cap = (m_max_db[idx] * h) / SPECTRUM_RANGE_DB;
        if (cap > h)
            cap = h;
        if (cap > 0 && (x & 1) == 0)
            col |= 1ull << PLOT_BIT(bottom + 1 - cap, page0);

        // Reference trace as a sparse dashed line. It is not clamped, so a
        // value above the range lands above the plot like it always did.
        if (m_has_ref && (x & 3) == 0)
        {
            int r = (m_ref_db[idx] * h) / SPECTRUM_RANGE_DB;
            if (r > h)
                gfx_pixel(x, bottom + 1 - r, true);
            else if (r > 0)
                col |= 1ull << PLOT_BIT(bottom + 1 - r, page0);
        }

        for (int p = 0; p < pages; p++)
            fb[p * DISP_W + x] |= (uint8_t)(col >> (p * 8));
    }

    // Marker as a dotted line that inverts whatever is under it, the delta
    // reference marker as a dashed one
    for (int m = 0; m < 2; m++)
    {
        int mcol = m == 0 ? marker_col : delta_col;
        if (mcol < 0 || mcol >= DISP_W)
            continue;

        uint64_t dots = 0;
        for (int y = top; y <= bottom; y++)
        {
            bool on = m == 0 ? ((y - top) & 1) == 0 : ((y - top) & 3) < 2;
            if (on)
                dots |= 1ull << PLOT_BIT(y, page0);
        }
        for (int p = 0; p < pages; p++)
            fb[p * DISP_W + mcol] ^= (uint8_t)(dots >> (p * 8));
    }
}

void spectrum_draw(int marker_col)
{
    spectrum_draw_plot(SPECTRUM_TOP, SPECTRUM_H, marker_col, -1);
}

// Micro font text that inverts whatever is under it
static void text_micro_xor(int x, int y, const char *str)
{
    while (*str)
    {
        const uint8_t *g = font3x5_glyph(*str++);
        for (int i = 0; i < MICRO_WIDTH; i++)
        {
            for (int j = 0; j < MICRO_HEIGHT; j++)
            {
                if (g[i] & (1 << j))
                    gfx_pixel(x + i, y + j, !gfx_pixel_get(x + i, y + j));
            }
        }
        x += MICRO_ADVANCE;
    }
}

void spectrum_draw_db_labels(int top, int h)
{
    // The grid is dB over the tracked floor, which differs per channel. The
    // labels put the mean floor under it, so they read true to a few dB.
    int floor_dbm = spectrum_dbm(spectrum_floor_rssi());
    int bottom = top + h - 1;
    char buf[8];

    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = bottom - (db * h) / SPECTRUM_RANGE_DB;
        gfx_fmt_int(buf, floor_dbm + db);
        text_micro_xor(1, y - MICRO_HEIGHT / 2, buf);
    }
}

// Spreads the level flags of one history byte (bits 0, 2, 4, 6 for its four
// columns) to bit 0 of four bytes, column 4k + c ending up in byte c. The
// partial products never meet at bits 0, 8, 16 or 24, so no carry spoils them.
static inline uint32_t spread4(uint32_t flags)
{
    return (flags * 0x41041u) & 0x01010101u;
}

void spectrum_draw_waterfall_rows(int top, int rows, uint16_t scroll_back)
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

    if (top < 0 || rows < 1 || top + rows > DISP_H)
        return;

    int y_end = top + rows;
    for (int page = top / 8; page * 8 < y_end; page++)
    {
        // Four columns per word, one byte each, bit (y % 8) per row
        uint32_t acc[ROW_BYTES] = {0};
        bool any = false;

        int y0 = page * 8 < top ? top : page * 8;
        int y1 = page * 8 + 8 < y_end ? page * 8 + 8 : y_end;
        for (int y = y0; y < y1; y++)
        {
            uint16_t rows_back = (uint16_t)(scroll_back + (y - top));
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

void spectrum_draw_waterfall(uint16_t scroll_back)
{
    spectrum_draw_waterfall_rows(WATERFALL_START, WATERFALL_ROWS, scroll_back);
}

void spectrum_draw_ruler_at(int ruler_y, uint8_t plan, int marker_col)
{
    if (ruler_y < 0 || ruler_y >= DISP_H)
        return;

    // A dedicated row so labels never sit on top of the data
    uint8_t *fb = &g_frame_buffer[(ruler_y / 8) * DISP_W];
    for (int x = 0; x < DISP_W; x++)
        fb[x] |= (uint8_t)(1u << (ruler_y % 8));

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
            gfx_pixel(x0, ruler_y + 1, true);
            gfx_pixel(x1, ruler_y + 1, true);
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
        gfx_text_micro(lx, ruler_y + 2, label);
        last_end = lx + w - 1;
    }

    if (marker_col >= 0 && marker_col < DISP_W)
        gfx_pixel(marker_col, ruler_y + 1, true);
}

void spectrum_draw_ruler(uint8_t plan, int marker_col)
{
    spectrum_draw_ruler_at(RULER_Y, plan, marker_col);
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

// A local maximum of v that is high enough and stands out enough. Plateaus
// count once, at their first channel.
static bool is_peak(const uint8_t *v, uint8_t n, uint8_t i)
{
    uint8_t top = v[i];
    if (top < PEAK_MIN_DB)
        return false;
    if (i > 0 && v[i - 1] >= top)
        return false;
    if (i + 1 < n && v[i + 1] > top)
        return false;

    // Prominence: walk each way until higher ground and keep the lowest point
    // on the way. The key col is the higher of the two lows that lead to
    // higher ground; with none either way this is the highest peak. Between
    // equal heights the one further left counts as higher, so the ripple on
    // top of a wide signal does not make every crest a peak of its own.
    int key = -1;
    for (int dir = -1; dir <= 1; dir += 2)
    {
        uint8_t low = top;
        for (int j = i + dir; j >= 0 && j < n; j += dir)
        {
            if (v[j] > top || (dir < 0 && v[j] == top))
            {
                if (low > key)
                    key = low;
                break;
            }
            if (v[j] < low)
                low = v[j];
        }
    }
    return key < 0 || top - key >= PEAK_PROMINENCE_DB;
}

bool spectrum_find_peak(uint8_t rank, uint8_t *chan_index)
{
    uint8_t count = scanner_count();
    uint8_t v[SCAN_MAX_CHANNELS];
    uint8_t peaks[SCAN_MAX_CHANNELS];
    uint8_t n = 0;

    for (uint8_t i = 0; i < count; i++)
        v[i] = spectrum_trace_db(i);
    for (uint8_t i = 0; i < count; i++)
    {
        if (is_peak(v, count, i))
            peaks[n++] = i;
    }
    if (rank >= n)
        return false;

    // Partial selection sort, strongest first, lower channel first on a tie
    for (uint8_t k = 0; k <= rank; k++)
    {
        uint8_t best = k;
        for (uint8_t j = k + 1; j < n; j++)
        {
            if (v[peaks[j]] > v[peaks[best]])
                best = j;
        }
        uint8_t t = peaks[k];
        peaks[k] = peaks[best];
        peaks[best] = t;
    }
    *chan_index = peaks[rank];
    return true;
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

            // Long window occupancy first, the max hold breaks ties between
            // channels that were never busy
            uint16_t score = (uint16_t)((m_occ_q8[i] >> 8) * 4u + m_max_db[i]);
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
