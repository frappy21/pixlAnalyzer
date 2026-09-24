/** 5x7 pixel font, ASCII 32 ('space') .. 122 ('z'), column major. */
#ifndef PIXLA_FONT5X7_H
#define PIXLA_FONT5X7_H

#include <stdint.h>

#define FONT_FIRST_CHAR 32
#define FONT_LAST_CHAR 122
#define FONT_WIDTH 5
#define FONT_ADVANCE 6

extern const uint8_t font5x7[][FONT_WIDTH];

#endif // PIXLA_FONT5X7_H
