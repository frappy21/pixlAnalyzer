#include "gfx.h"

#include "display.h"
#include "font3x5.h"
#include "font5x7.h"

void gfx_pixel(int x, int y, bool on)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H)
    {
        if (on)
            g_frame_buffer[x + (y / 8) * DISP_W] |= (1 << (y % 8));
        else
            g_frame_buffer[x + (y / 8) * DISP_W] &= ~(1 << (y % 8));
    }
}

bool gfx_pixel_get(int x, int y)
{
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H)
        return false;

    return (g_frame_buffer[x + (y / 8) * DISP_W] & (1 << (y % 8))) != 0;
}

void gfx_vline(int x, int y1, int y2)
{
    if (y1 > y2)
    {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    for (int y = y1; y <= y2; y++)
        gfx_pixel(x, y, true);
}

void gfx_hline(int x1, int x2, int y)
{
    if (x1 > x2)
    {
        int t = x1;
        x1 = x2;
        x2 = t;
    }
    for (int x = x1; x <= x2; x++)
        gfx_pixel(x, y, true);
}

void gfx_box(int x, int y, int w, int h, bool fill, bool color)
{
    for (int i = x; i < x + w; i++)
    {
        for (int j = y; j < y + h; j++)
        {
            if (i == x || i == x + w - 1 || j == y || j == y + h - 1 || fill)
            {
                gfx_pixel(i, j, color);
            }
        }
    }
}

void gfx_invert(int x, int y, int w, int h)
{
    for (int i = x; i < x + w; i++)
        for (int j = y; j < y + h; j++)
            gfx_pixel(i, j, !gfx_pixel_get(i, j));
}

void gfx_char(int x, int y, char c)
{
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
        c = ' ';
    c -= FONT_FIRST_CHAR;
    for (int i = 0; i < FONT_WIDTH; i++)
    {
        uint8_t line = font5x7[(int)c][i];
        for (int j = 0; j < 8; j++)
        {
            if (line & (1 << j))
                gfx_pixel(x + i, y + j, true);
        }
    }
}

void gfx_text(int x, int y, const char *str)
{
    while (*str)
    {
        gfx_char(x, y, *str++);
        x += FONT_ADVANCE;
    }
}

int gfx_text_width(const char *str)
{
    int n = 0;
    while (str[n])
        n++;
    return n * FONT_ADVANCE - 1;
}

void gfx_text_bg(int x, int y, const char *str)
{
    // Clear a halo so the glyphs stay legible on top of spectrum or waterfall
    gfx_box(x - 1, y - 1, gfx_text_width(str) + 2, 9, true, false);
    gfx_text(x, y, str);
}

void gfx_text_micro(int x, int y, const char *str)
{
    while (*str)
    {
        const uint8_t *g = font3x5_glyph(*str++);
        for (int i = 0; i < MICRO_WIDTH; i++)
        {
            for (int j = 0; j < MICRO_HEIGHT; j++)
            {
                if (g[i] & (1 << j))
                    gfx_pixel(x + i, y + j, true);
            }
        }
        x += MICRO_ADVANCE;
    }
}

int gfx_text_micro_width(const char *str)
{
    int n = 0;
    while (str[n])
        n++;
    return n * MICRO_ADVANCE - 1;
}

void gfx_dither_pixel(int x, int y, uint8_t level)
{
    // Ordered 2x2 Bayer matrix, level 0..3
    static const uint8_t bayer[2][2] = {{0, 2}, {3, 1}};

    if (level > bayer[x & 1][y & 1])
        gfx_pixel(x, y, true);
}

char *gfx_fmt_int(char *buf, int value)
{
    char tmp[12];
    int n = 0;
    bool neg = value < 0;
    unsigned v = neg ? (unsigned)(-value) : (unsigned)value;

    do
    {
        tmp[n++] = '0' + (v % 10);
        v /= 10;
    } while (v);

    char *p = buf;
    if (neg)
        *p++ = '-';
    while (n)
        *p++ = tmp[--n];
    *p = '\0';
    return buf;
}

char *gfx_fmt_fixed(char *buf, int value, int decimals)
{
    int scale = 1;
    for (int i = 0; i < decimals; i++)
        scale *= 10;

    bool neg = value < 0;
    if (neg)
        value = -value;

    char *p = buf;
    if (neg)
        *p++ = '-';

    p = gfx_fmt_int(p, value / scale);
    while (*p)
        p++;

    if (decimals > 0)
    {
        *p++ = '.';
        int frac = value % scale;
        for (int i = decimals - 1; i >= 0; i--)
        {
            int d = frac;
            for (int k = 0; k < i; k++)
                d /= 10;
            *p++ = '0' + (d % 10);
        }
        *p = '\0';
    }
    return buf;
}
