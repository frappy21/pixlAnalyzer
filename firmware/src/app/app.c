#include "app.h"

#include "board_config.h"
#include "buttons.h"
#include "display.h"
#include "gfx.h"
#include "screens.h"
#include "settings.h"
#include "systime.h"

uint8_t g_app_arena[APP_ARENA_SIZE] __attribute__((aligned(4)));

// How long the name of a main screen stays up after switching to it
#define BANNER_MS 700

static const app_screen_t *m_stack[APP_STACK_DEPTH];
static uint8_t m_depth;
static bool m_redraw = true;
static uint8_t m_home; // index into g_app_home

static const char *m_banner;
static uint32_t m_banner_until;

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

void app_init(void)
{
    const app_screen_t *home = g_app_home[0];

    m_boot_ms = systime_ms();
    m_last_input_ms = m_boot_ms;

    m_home = 0;
    m_stack[0] = home;
    m_depth = 1;
    m_redraw = true;
    if (home->enter)
        home->enter();
}

// ---------------------------------------------------------------------------
// Carousel
// ---------------------------------------------------------------------------

static int home_index(const app_screen_t *screen)
{
    for (uint8_t i = 0; i < g_app_home_count; i++)
    {
        if (g_app_home[i] == screen)
            return i;
    }
    return -1;
}

bool app_is_main(const app_screen_t *screen)
{
    return home_index(screen) >= 0;
}

// Name of the new main screen in a framed box in the middle, drawn by every
// flush while it is up. The category (its menu group) rides along in the
// micro font, so a spin through the carousel reads "RADIOS / BLE".
static const char *home_category(const app_screen_t *screen)
{
    switch (screen->group)
    {
    case APP_GROUP_SPECTRUM:
        return "RF TOOLS";
    case APP_GROUP_RECEIVE:
        return "RADIOS";
    case APP_GROUP_RC:
        return "RC + DRONES";
    case APP_GROUP_NFC:
        return "NFC";
    case APP_GROUP_TRANSMIT:
        return "TX LAB";
    default:
        return 0;
    }
}

static void banner_draw(void)
{
    const char *cat = home_category(m_stack[0]);
    int box_h = cat ? 18 : 13;
    int box_y = cat ? 20 : 22;
    int w = gfx_text_width(m_banner) + 8;
    if (cat)
    {
        int cw = gfx_text_micro_width(cat) + 8;
        if (cw > w)
            w = cw;
    }
    int x = (DISP_W - w) / 2;
    gfx_box(x, box_y, w, box_h, true, false);
    gfx_box(x, box_y, w, box_h, false, true);
    if (cat)
    {
        gfx_text_micro(x + 4, box_y + 3, cat);
        gfx_text(x + 4, box_y + 9, m_banner);
    }
    else
        gfx_text(x + 4, box_y + 3, m_banner);
}

static void home_set(uint8_t index)
{
    while (m_depth > 1)
        pop();

    if (index != m_home)
    {
        const app_screen_t *old = m_stack[0];
        if (old->leave)
            old->leave();

        m_home = index;
        m_stack[0] = g_app_home[index];
        if (m_stack[0]->enter)
            m_stack[0]->enter();
    }

    m_banner = g_app_home_label[index];
    m_banner_until = systime_ms() + BANNER_MS;
    display_set_overlay(banner_draw);
    transition();
}

void app_home_switch(int direction)
{
    uint8_t n = g_app_home_count;
    home_set((uint8_t)((m_home + n + (direction < 0 ? -1 : 1)) % n));
}

void app_home_select(const app_screen_t *screen)
{
    int i = home_index(screen);
    if (i >= 0)
        home_set((uint8_t)i);
}

void app_banner_update(uint32_t now)
{
    if (m_banner && (int32_t)(now - m_banner_until) >= 0)
    {
        m_banner = 0;
        display_set_overlay(0);
        m_redraw = true;
    }
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

    if (app_is_main(screen))
    {
        app_home_select(screen);
        return;
    }

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
    // A long RIGHT on a main screen switches screens, so there it is a click
    // on release, like LEFT
    if (app_at_home())
        return noted(buttons_clicked(BTN_RIGHT));
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
