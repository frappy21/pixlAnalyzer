#include "app.h"

#include "buttons.h"
#include "display.h"
#include "settings.h"
#include "systime.h"

uint8_t g_app_arena[APP_ARENA_SIZE] __attribute__((aligned(4)));

static const app_screen_t *m_stack[APP_STACK_DEPTH];
static uint8_t m_depth;
static bool m_redraw = true;

static uint32_t m_boot_ms;
static uint32_t m_last_input_ms;
static bool m_dimmed;

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

static void transition(void)
{
    m_redraw = true;

    // Whatever is still held belongs to the screen we are leaving: the new
    // one only sees buttons pressed after they have been released
    buttons_flush();
}

static void pop(void)
{
    const app_screen_t *top = m_stack[m_depth - 1];
    if (top->leave)
        top->leave();
    m_depth--;
}

void app_init(const app_screen_t *home)
{
    m_boot_ms = systime_ms();
    m_last_input_ms = m_boot_ms;

    m_stack[0] = home;
    m_depth = 1;
    m_redraw = true;
    if (home->enter)
        home->enter();
}

const app_screen_t *app_current(void)
{
    return m_stack[m_depth - 1];
}

bool app_at_home(void)
{
    return m_depth <= 1;
}

void app_open(const app_screen_t *screen)
{
    if (!screen || !screen->tick)
        return;

    if (m_depth >= APP_STACK_DEPTH)
        pop();

    m_stack[m_depth++] = screen;
    if (screen->enter)
        screen->enter();
    transition();
}

void app_back(void)
{
    if (m_depth <= 1)
        return;

    pop();
    transition();
}

void app_home(void)
{
    while (m_depth > 1)
        pop();
    transition();
}

// ---------------------------------------------------------------------------
// Redraw
// ---------------------------------------------------------------------------

void app_redraw(void)
{
    m_redraw = true;
}

bool app_take_redraw(void)
{
    bool r = m_redraw;
    m_redraw = false;
    return r;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void app_note_input(void)
{
    m_last_input_ms = systime_ms();
    if (m_dimmed)
    {
        display_set_backlight(g_settings.backlight);
        m_dimmed = false;
    }
}

static bool noted(bool event)
{
    if (event)
        app_note_input();
    return event;
}

bool app_left(void)
{
    return noted(buttons_clicked(BTN_LEFT));
}

bool app_right(void)
{
    return noted(buttons_repeat(BTN_RIGHT));
}

bool app_ok(void)
{
    return noted(buttons_clicked(BTN_MID));
}

bool app_ok_long(void)
{
    return noted(buttons_long(BTN_MID));
}

bool app_any(void)
{
    // All three are read, so no stale press is left behind
    bool left = buttons_pressed(BTN_LEFT);
    bool mid = buttons_pressed(BTN_MID);
    bool right = buttons_pressed(BTN_RIGHT);
    return noted(left || mid || right);
}

// ---------------------------------------------------------------------------
// Housekeeping state
// ---------------------------------------------------------------------------

uint32_t app_boot_ms(void)
{
    return m_boot_ms;
}

uint32_t app_last_input_ms(void)
{
    return m_last_input_ms;
}

bool app_dimmed(void)
{
    return m_dimmed;
}

void app_dim(void)
{
    display_set_backlight(g_settings.backlight / 6);
    m_dimmed = true;
}
