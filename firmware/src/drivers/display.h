/**
 * Display driver for the two Pixl.js panels, over SPIM with EasyDMA.
 * ST7565/ST7567 by default, SH1106 when OLED_TYPE_SH1106 is defined.
 *
 * display_flush() only sends the pages that actually changed since the last
 * flush, so a mostly static screen costs almost nothing.
 */
#ifndef PIXLA_DISPLAY_H
#define PIXLA_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// Frame buffer in the native page format of the controller:
// byte index = x + (y / 8) * DISP_W, bit index = y % 8
extern uint8_t g_frame_buffer[DISP_BUF_SIZE];

void display_init(void);
void display_uninit(void);

// Clears the frame buffer only, use display_flush() to push it to the panel
void display_clear(void);
void display_flush(void);
void display_flush_all(void); // ignore the dirty page tracking

// 0..63 on both panels, mapped onto the controller's own contrast range
void display_set_contrast(uint8_t value);

// 0..255, PWM on the LCD backlight pin. The OLED ignores it.
void display_set_backlight(uint8_t level);

void display_set_inverted(bool inverted);

#endif // PIXLA_DISPLAY_H
