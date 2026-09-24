#include "nrf_gpio.h"

#include "board_config.h"
#include "buttons.h"
#include "systime.h"

// A press has to stay stable this long before it counts. Contact bounce on
// these tactile switches settles well under 10ms.
#define DEBOUNCE_MS 20

// Auto repeat for navigation: first repeat after the delay, then every period
#define REPEAT_DELAY_MS 400
#define REPEAT_PERIOD_MS 140

typedef struct
{
    uint8_t pin;
    bool raw;           // last sampled level, active low already inverted
    bool stable;        // debounced level
    bool pressed_event; // set on the falling (press) edge, cleared when read
    bool ignore;        // press was consumed elsewhere, wait for release
    uint32_t changed_at;
    uint32_t pressed_at;
    uint32_t next_repeat;
} button_state_t;

static button_state_t m_buttons[BTN_COUNT] = {
    {.pin = PIN_BTN_LEFT},
    {.pin = PIN_BTN_MID},
    {.pin = PIN_BTN_RIGHT},
};

void buttons_init(void)
{
    for (int i = 0; i < BTN_COUNT; i++)
    {
        nrf_gpio_cfg_input(m_buttons[i].pin, NRF_GPIO_PIN_PULLUP);
    }
    nrf_gpio_cfg_input(PIN_CHRG_STAT, NRF_GPIO_PIN_PULLUP);

    // Start from the real pin state so a button that is already held (the wake
    // press after SYSTEM OFF) counts as down without generating a press event.
    uint32_t now = systime_ms();
    for (int i = 0; i < BTN_COUNT; i++)
    {
        bool level = (nrf_gpio_pin_read(m_buttons[i].pin) == 0);
        m_buttons[i].raw = level;
        m_buttons[i].stable = level;
        m_buttons[i].ignore = false;
        m_buttons[i].changed_at = now;
        m_buttons[i].pressed_at = now;
    }
}

void buttons_poll(void)
{
    uint32_t now = systime_ms();

    for (int i = 0; i < BTN_COUNT; i++)
    {
        button_state_t *b = &m_buttons[i];
        bool level = (nrf_gpio_pin_read(b->pin) == 0);

        if (level != b->raw)
        {
            // Bouncing, restart the settling window
            b->raw = level;
            b->changed_at = now;
            continue;
        }

        if (level == b->stable || (now - b->changed_at) < DEBOUNCE_MS)
        {
            continue;
        }

        // Stable for long enough, accept the new level
        b->stable = level;
        if (level)
        {
            b->pressed_at = now;
            b->next_repeat = now + REPEAT_DELAY_MS;
            b->pressed_event = !b->ignore;
        }
        else
        {
            b->ignore = false;
        }
    }
}

bool buttons_down(button_t btn)
{
    return m_buttons[btn].stable && !m_buttons[btn].ignore;
}

bool buttons_any_down(void)
{
    for (int i = 0; i < BTN_COUNT; i++)
    {
        if (buttons_down((button_t)i))
            return true;
    }
    return false;
}

bool buttons_pressed(button_t btn)
{
    if (!m_buttons[btn].pressed_event)
        return false;

    m_buttons[btn].pressed_event = false;
    return true;
}

bool buttons_repeat(button_t btn)
{
    button_state_t *b = &m_buttons[btn];

    if (buttons_pressed(btn))
        return true;

    if (!buttons_down(btn))
        return false;

    uint32_t now = systime_ms();
    if ((int32_t)(now - b->next_repeat) < 0)
        return false;

    b->next_repeat = now + REPEAT_PERIOD_MS;
    return true;
}

uint32_t buttons_held_ms(button_t btn)
{
    if (!buttons_down(btn))
        return 0;

    return systime_ms() - m_buttons[btn].pressed_at;
}

void buttons_flush(void)
{
    for (int i = 0; i < BTN_COUNT; i++)
    {
        m_buttons[i].pressed_event = false;
        m_buttons[i].ignore = m_buttons[i].stable;
    }
}
