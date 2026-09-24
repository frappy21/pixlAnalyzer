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

static uint8_t m_live_db[SCAN_MAX_CHANNELS];
static uint8_t m_peak_db[SCAN_MAX_CHANNELS];
static uint8_t m_max_db[SCAN_MAX_CHANNELS];
static uint8_t m_ref_db[SCAN_MAX_CHANNELS];
static bool m_has_ref;

// Waterfall history ring, newest row at m_head - 1
static uint8_t m_history[HISTORY_ROWS][ROW_BYTES];
static uint16_t m_head;
static uint16_t m_rows;

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

uint8_t spectrum_col_to_chan(int col)
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

static uint8_t history_level(uint16_t rows_back, int col)
{
    if (rows_back >= m_rows)
        return 0;

    uint16_t idx = (uint16_t)((m_head + HISTORY_ROWS - 1 - rows_back) % HISTORY_ROWS);
    uint8_t packed = m_history[idx][col / 4];
    return (packed >> ((col % 4) * 2)) & 3;
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
    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = spectrum_col_to_chan(x);
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

void spectrum_draw(int marker_col)
{
    // dB grid
    for (int db = DB_PER_GRID; db < SPECTRUM_RANGE_DB; db += DB_PER_GRID)
    {
        int y = SPECTRUM_TOP + SPECTRUM_H - 1 - (db * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        for (int x = 0; x < DISP_W; x += 8)
            gfx_pixel(x, y, true);
    }

    for (int x = 0; x < DISP_W; x++)
    {
        uint8_t idx = spectrum_col_to_chan(x);

        int bar = (m_peak_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (bar > SPECTRUM_H)
            bar = SPECTRUM_H;
        if (bar > 0)
            gfx_vline(x, SPECTRUM_TOP + SPECTRUM_H - bar, SPECTRUM_TOP + SPECTRUM_H - 1);

        // Max hold as a dotted cap above the live bar
        int cap = (m_max_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
        if (cap > SPECTRUM_H)
            cap = SPECTRUM_H;
        if (cap > 0 && (x & 1) == 0)
            gfx_pixel(x, SPECTRUM_TOP + SPECTRUM_H - cap, true);

        // Reference trace as a sparse dashed line
        if (m_has_ref && (x & 3) == 0)
        {
            int r = (m_ref_db[idx] * SPECTRUM_H) / SPECTRUM_RANGE_DB;
            if (r > 0)
                gfx_pixel(x, SPECTRUM_TOP + SPECTRUM_H - r, true);
        }
    }

    if (marker_col >= 0 && marker_col < DISP_W)
    {
        for (int y = SPECTRUM_TOP; y < SPECTRUM_TOP + SPECTRUM_H; y += 2)
            gfx_pixel(marker_col, y, !gfx_pixel_get(marker_col, y));
    }
}

void spectrum_draw_waterfall(uint16_t scroll_back)
{
    bool dither = (g_settings.wf_mode == WF_DITHER);

    for (uint16_t r = 0; r < WATERFALL_ROWS; r++)
    {
        int y = WATERFALL_START + r;
        for (int x = 0; x < DISP_W; x++)
        {
            uint8_t level = history_level(scroll_back + r, x);
            if (level == 0)
                continue;

            if (dither)
                gfx_dither_pixel(x, y, level);
            else if (level >= 2)
                gfx_pixel(x, y, true);
        }
    }
}

void spectrum_draw_ruler(uint8_t plan, int marker_col)
{
    // A dedicated row so labels never sit on top of the data
    for (int x = 0; x < DISP_W; x++)
        gfx_pixel(x, RULER_Y, true);

    if (plan == PLAN_NONE)
        return;

    uint16_t lo = scanner_span_start();
    uint16_t hi = scanner_span_end();
    if (hi <= lo)
        return;

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

        int lx = cx - gfx_text_micro_width(label) / 2;
        if (lx < 0)
            lx = 0;
        if (lx + gfx_text_micro_width(label) >= DISP_W)
            lx = DISP_W - 1 - gfx_text_micro_width(label);
        gfx_text_micro(lx, RULER_Y + 2, label);
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
