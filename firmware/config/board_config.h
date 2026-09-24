/**
 * Board level configuration: pin map of the Pixl.js hardware (LCD and OLED
 * revisions share the same pinout) and geometry of the attached display.
 *
 * Cross checked against solosky/pixl.js: fw/application/src/boards/board_lcd.h
 * and board_oled.h.
 */
#ifndef PIXLA_BOARD_CONFIG_H
#define PIXLA_BOARD_CONFIG_H

// Display (SPI, ST7565/ST7567 or SH1106)
#define PIN_LCD_SCL 26
#define PIN_LCD_MOSI 25
#define PIN_LCD_MISO 19 // not used by the panel, needed by the SPI NOR flash
#define PIN_LCD_CS 27
#define PIN_LCD_DC 28
#define PIN_LCD_RST 29
#define PIN_LCD_BL 30

// 2MB SPI NOR flash (GD25Q16C), shares SCK/MOSI/MISO with the display
#define PIN_FLASH_CS 18

// Buttons & analog inputs
#define PIN_BTN_LEFT 5
#define PIN_BTN_MID 6
#define PIN_BTN_RIGHT 7
#define PIN_ADC_INPUT 2
#define PIN_CHRG_STAT 3

// Status LED, active low, unused by the stock analyzer firmware
#define PIN_LED 31

// Display geometry
#define DISP_W 128
#define DISP_H 64
#define DISP_PAGES (DISP_H / 8)
#define DISP_BUF_SIZE (DISP_W * DISP_PAGES)

#endif // PIXLA_BOARD_CONFIG_H
