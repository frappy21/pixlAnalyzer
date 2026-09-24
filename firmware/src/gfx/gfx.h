/** Drawing primitives operating on the display frame buffer. */
#ifndef PIXLA_GFX_H
#define PIXLA_GFX_H

#include <stdbool.h>
#include <stdint.h>

void gfx_pixel(int x, int y, bool on);
bool gfx_pixel_get(int x, int y);
void gfx_vline(int x, int y1, int y2);
void gfx_hline(int x1, int x2, int y);
void gfx_box(int x, int y, int w, int h, bool fill, bool color);
void gfx_invert(int x, int y, int w, int h);

// 5x7 font
void gfx_char(int x, int y, char c);
void gfx_text(int x, int y, const char *str);
// Same, but clears a one pixel halo first so text stays readable over data
void gfx_text_bg(int x, int y, const char *str);
int gfx_text_width(const char *str);

// 3x5 micro font
void gfx_text_micro(int x, int y, const char *str);
int gfx_text_micro_width(const char *str);

// Ordered 2x2 dither: level 0 draws nothing, level 3 draws solid
void gfx_dither_pixel(int x, int y, uint8_t level);

// Integer formatting helpers, so the render path needs no printf
char *gfx_fmt_int(char *buf, int value);
char *gfx_fmt_fixed(char *buf, int value, int decimals); // value scaled by 10^decimals

#endif // PIXLA_GFX_H
