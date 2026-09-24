#include "nrf_gpio.h"

#include "board_config.h"
#include "led.h"

#define LED_ON_MS 12

static uint8_t m_rate;
static uint32_t m_next_ms;
static bool m_on;

void led_init(void)
{
    nrf_gpio_cfg_output(PIN_LED);
    nrf_gpio_pin_set(PIN_LED); // active low
    m_rate = 0;
    m_on = false;
}

void led_set(bool on)
{
    if (on)
        nrf_gpio_pin_clear(PIN_LED);
    else
        nrf_gpio_pin_set(PIN_LED);
    m_on = on;
}

void led_off(void)
{
    m_rate = 0;
    led_set(false);
}

void led_set_rate(uint8_t hits_per_second)
{
    m_rate = hits_per_second;
    if (m_rate == 0)
        led_set(false);
}

void led_update(uint32_t now_ms)
{
    if (m_rate == 0)
        return;

    if (m_rate == 255)
    {
        led_set(true);
        return;
    }

    if (m_on)
    {
        if ((int32_t)(now_ms - m_next_ms) >= 0)
        {
            led_set(false);
            m_next_ms = now_ms + (1000 / m_rate) - LED_ON_MS;
        }
        return;
    }

    if ((int32_t)(now_ms - m_next_ms) >= 0)
    {
        led_set(true);
        m_next_ms = now_ms + LED_ON_MS;
    }
}
