#include "font3x5.h"

// Digits 0..9
static const uint8_t digits[10][3] = {
    {31, 17, 31}, // 0
    {18, 31, 16}, // 1
    {29, 21, 23}, // 2
    {21, 21, 31}, // 3
    {7, 4, 31},   // 4
    {23, 21, 29}, // 5
    {31, 21, 29}, // 6
    {1, 1, 31},   // 7
    {31, 21, 31}, // 8
    {23, 21, 31}, // 9
};

// Capitals A..Z
static const uint8_t letters[26][3] = {
    {31, 5, 31},  // A
    {31, 21, 14}, // B
    {31, 17, 17}, // C
    {31, 17, 14}, // D
    {31, 21, 21}, // E
    {31, 5, 5},   // F
    {31, 17, 29}, // G
    {31, 4, 31},  // H
    {17, 31, 17}, // I
    {24, 16, 31}, // J
    {31, 4, 27},  // K
    {31, 16, 16}, // L
    {31, 2, 31},  // M
    {31, 6, 31},  // N
    {31, 17, 31}, // O
    {31, 5, 7},   // P
    {15, 9, 31},  // Q
    {31, 5, 23},  // R
    {23, 21, 29}, // S
    {1, 31, 1},   // T
    {31, 16, 31}, // U
    {15, 16, 15}, // V
    {31, 8, 31},  // W
    {27, 4, 27},  // X
    {7, 28, 7},   // Y
    {25, 21, 19}, // Z
};

static const uint8_t blank[3] = {0, 0, 0};
static const uint8_t dash[3] = {4, 4, 4};
static const uint8_t dot[3] = {0, 16, 0};
static const uint8_t slash[3] = {16, 14, 1};
static const uint8_t plus[3] = {4, 14, 4};
static const uint8_t colon[3] = {0, 10, 0};

const uint8_t *font3x5_glyph(char c)
{
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'];
    if (c >= 'a' && c <= 'z')
        return letters[c - 'a'];

    switch (c)
    {
    case '-':
        return dash;
    case '.':
        return dot;
    case '/':
        return slash;
    case '+':
        return plus;
    case ':':
        return colon;
    default:
        return blank;
    }
}
