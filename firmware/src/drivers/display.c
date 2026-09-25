#include <string.h>

#include "nrf.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"

#include "display.h"
#include "spi_bus.h"

// Hardware Offset Definition:
#ifdef OLED_TYPE_SH1106
#define LCD_START_COL 2
#define CONTRAST_SCALE 4 // SH1106 takes 0..255
#else
#define LCD_START_COL 0
#define CONTRAST_SCALE 1 // ST7565 electronic volume is 0..63
#endif

uint8_t g_frame_buffer[DISP_BUF_SIZE];

static uint8_t m_shadow[DISP_BUF_SIZE];
static bool m_shadow_valid;

// Burn-in protection offset, 0 or 1 pixel right and down. OLED only.
static uint8_t m_shift_x;
static uint8_t m_shift_y;

static void lcd_cmd(uint8_t cmd)
{
    uint8_t b = cmd;
    nrf_gpio_pin_clear(PIN_LCD_DC);
    nrf_gpio_pin_clear(PIN_LCD_CS);
    spi_bus_write(&b, 1);
    nrf_gpio_pin_set(PIN_LCD_CS);
}

static void lcd_data(const uint8_t *data, int len)
{
    nrf_gpio_pin_set(PIN_LCD_DC);
    nrf_gpio_pin_clear(PIN_LCD_CS);
    spi_bus_write(data, len);
    nrf_gpio_pin_set(PIN_LCD_CS);
}

// Duty cycle word for the PWM sequence, must live in RAM for EasyDMA
static uint16_t m_bl_seq[1] = {0x8000};

static void backlight_init(void)
{
    // PWM0 drives the backlight so it can be dimmed instead of only on/off
    nrf_gpio_cfg_output(PIN_LCD_BL);
    nrf_gpio_pin_clear(PIN_LCD_BL);

    NRF_PWM0->PSEL.OUT[0] = PIN_LCD_BL;
    NRF_PWM0->PSEL.OUT[1] = 0xFFFFFFFF;
    NRF_PWM0->PSEL.OUT[2] = 0xFFFFFFFF;
    NRF_PWM0->PSEL.OUT[3] = 0xFFFFFFFF;
    NRF_PWM0->MODE = PWM_MODE_UPDOWN_Up;
    NRF_PWM0->PRESCALER = PWM_PRESCALER_PRESCALER_DIV_16; // 1MHz
    NRF_PWM0->COUNTERTOP = 256;                           // ~3.9kHz, no flicker
    NRF_PWM0->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) |
                        (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);

    // Both sequences point at the same value and the loop restarts itself, so
    // the duty cycle holds until we change it
    for (int i = 0; i < 2; i++)
    {
        NRF_PWM0->SEQ[i].PTR = (uint32_t)m_bl_seq;
        NRF_PWM0->SEQ[i].CNT = 1;
        NRF_PWM0->SEQ[i].REFRESH = 0;
        NRF_PWM0->SEQ[i].ENDDELAY = 0;
    }
    NRF_PWM0->LOOP = 0xFFFF;
    NRF_PWM0->SHORTS = PWM_SHORTS_LOOPSDONE_SEQSTART0_Msk;
    NRF_PWM0->ENABLE = 1;
    NRF_PWM0->TASKS_SEQSTART[0] = 1;
}

void display_set_backlight(uint8_t level)
{
    // Bit 15 picks the polarity where the output is low until the compare and
    // high afterwards, so the on time is COUNTERTOP - level: a level of 255
    // has to become a compare value of 1, not 255.
    m_bl_seq[0] = 0x8000 | (uint16_t)(256 - level);
    NRF_PWM0->TASKS_SEQSTART[0] = 1;
}

void display_set_contrast(uint8_t value)
{
    if (value > 63)
        value = 63;

    lcd_cmd(0x81);
    lcd_cmd(value * CONTRAST_SCALE);
}

void display_set_inverted(bool inverted)
{
    lcd_cmd(inverted ? 0xA7 : 0xA6);
}

void display_set_panel_off(bool off)
{
    // Both controllers: 0xAE display off (RAM kept), 0xAF display on
    lcd_cmd(off ? 0xAE : 0xAF);
}

void display_set_shift(uint8_t dx, uint8_t dy)
{
#ifdef OLED_TYPE_SH1106
    dx = dx ? 1 : 0;
    dy = dy ? 1 : 0;
    if (dx != m_shift_x || dy != m_shift_y)
    {
        m_shift_x = dx;
        m_shift_y = dy;
        m_shadow_valid = false; // every page moves
    }
#else
    (void)dx;
    (void)dy;
#endif
}

void display_init(void)
{
    nrf_gpio_cfg_output(PIN_LCD_CS);
    nrf_gpio_cfg_output(PIN_LCD_DC);
    nrf_gpio_cfg_output(PIN_LCD_RST);
    nrf_gpio_pin_set(PIN_LCD_CS);

    // The flash chip shares SCK and MOSI, keep it deselected while we talk
    nrf_gpio_cfg_output(PIN_FLASH_CS);
    nrf_gpio_pin_set(PIN_FLASH_CS);

    spi_bus_init();
    backlight_init();

    nrf_gpio_pin_clear(PIN_LCD_RST);
    nrf_delay_ms(100);
    nrf_gpio_pin_set(PIN_LCD_RST);
    nrf_delay_ms(100);

#ifdef OLED_TYPE_SH1106
    lcd_cmd(0xAE);
    lcd_cmd(0x00 | (LCD_START_COL & 0x0F));
    lcd_cmd(0x10 | (LCD_START_COL >> 4));
    lcd_cmd(0x40);
    lcd_cmd(0xB0);
    lcd_cmd(0x81);
    lcd_cmd(0xCF);
    lcd_cmd(0xA1);
    lcd_cmd(0xA6);
    lcd_cmd(0xA8);
    lcd_cmd(0x3F);
    lcd_cmd(0xAD);
    lcd_cmd(0x8B);
    lcd_cmd(0x33);
    lcd_cmd(0xC8);
    lcd_cmd(0xD3);
    lcd_cmd(0x00);
    lcd_cmd(0xD5);
    lcd_cmd(0x80);
    lcd_cmd(0xD9);
    lcd_cmd(0x1F);
    lcd_cmd(0xDA);
    lcd_cmd(0x12);
    lcd_cmd(0xDB);
    lcd_cmd(0x40);
    lcd_cmd(0xAF);
#else
    lcd_cmd(0xE2);
    nrf_delay_ms(10);
    lcd_cmd(0xA2);
    lcd_cmd(0xA0);
    lcd_cmd(0xC8);
    lcd_cmd(0x23);
    lcd_cmd(0x81);
    lcd_cmd(0x32);
    lcd_cmd(0x2F);
    lcd_cmd(0xB0);
    lcd_cmd(0xA6);
    lcd_cmd(0xAF);
#endif

    m_shadow_valid = false;
}

void display_uninit(void)
{
#ifdef OLED_TYPE_SH1106
    lcd_cmd(0xAE);
#endif
    display_set_backlight(0);
    NRF_PWM0->ENABLE = 0;
    NRF_PWM0->PSEL.OUT[0] = 0xFFFFFFFF;
    nrf_gpio_cfg_output(PIN_LCD_BL);
    nrf_gpio_pin_clear(PIN_LCD_BL);
    nrf_delay_ms(50);

    spi_bus_uninit();
    nrf_gpio_cfg_default(PIN_LCD_CS);
    nrf_gpio_cfg_default(PIN_LCD_DC);
    nrf_gpio_cfg_default(PIN_LCD_RST);
    nrf_gpio_cfg_default(PIN_LCD_BL);
    // Leave the flash chip select high so the NOR stays deselected in sleep
    nrf_gpio_cfg_output(PIN_FLASH_CS);
    nrf_gpio_pin_set(PIN_FLASH_CS);
}

void display_clear(void)
{
    memset(g_frame_buffer, 0, DISP_BUF_SIZE);
}

static void flush_page(uint8_t page, const uint8_t *data)
{
    lcd_cmd(0xB0 + page);
    lcd_cmd(0x00 | (LCD_START_COL & 0x0F));
    lcd_cmd(0x10 | ((LCD_START_COL >> 4) & 0x0F));
    lcd_data(data, DISP_W);
}

// One page of the frame moved m_shift_x right and m_shift_y down. The last
// column and row fall off the panel, the first ones come up blank.
static const uint8_t *shifted_page(uint8_t page, uint8_t *out)
{
    const uint8_t *src = &g_frame_buffer[page * DISP_W];
    const uint8_t *above = page ? src - DISP_W : 0;

    for (int x = 0; x < DISP_W; x++)
    {
        int sx = x - m_shift_x;
        if (sx < 0)
        {
            out[x] = 0;
            continue;
        }
        uint8_t v = src[sx];
        if (m_shift_y)
            v = (uint8_t)((v << 1) | (above ? above[sx] >> 7 : 0));
        out[x] = v;
    }
    return out;
}

static void (*m_overlay)(void);

void display_set_overlay(void (*draw)(void))
{
    m_overlay = draw;
}

void display_flush(void)
{
    if (m_overlay)
        m_overlay();

    uint8_t moved[DISP_W];
    bool shifted = m_shift_x || m_shift_y;

    // The shadow holds what the panel shows, shifted or not
    for (uint8_t page = 0; page < DISP_PAGES; page++)
    {
        const uint8_t *src = shifted ? shifted_page(page, moved) : &g_frame_buffer[page * DISP_W];
        uint8_t *dst = &m_shadow[page * DISP_W];

        if (m_shadow_valid && memcmp(src, dst, DISP_W) == 0)
            continue;

        flush_page(page, src);
        memcpy(dst, src, DISP_W);
    }
    m_shadow_valid = true;
}

void display_flush_all(void)
{
    m_shadow_valid = false;
    display_flush();
}
