/**
 * 3x5 pixel micro font: digits, capital letters and a few symbols.
 * Used where the 5x7 font does not fit, e.g. the channel ruler row.
 *
 * One byte per column, bit 0 is the top row.
 */
#ifndef PIXLA_FONT3X5_H
#define PIXLA_FONT3X5_H

#include <stdint.h>

#define MICRO_WIDTH 3
#define MICRO_ADVANCE 4
#define MICRO_HEIGHT 5

// Returns the three column bytes for c, or the blank glyph for anything
// outside the supported set.
const uint8_t *font3x5_glyph(char c);

#endif // PIXLA_FONT3X5_H
