/**
 * Menus, built from the screen registry: the top level menu (a row per
 * non-empty group, then the APP_GROUP_SYSTEM entries) and one submenu per
 * group. Every list starts with a Back row, although a long LEFT does the
 * same from anywhere.
 */
#include "screens.h"
#include "ui.h"

// Rows one menu can hold, Back row included
#define MENU_ROWS_MAX 32

typedef struct
{
    const char *label;
    const app_screen_t *screen; // entry to open or run, NULL for Back and group rows
    uint8_t group;              // group row: the group to open; Back row: APP_GROUP_HIDDEN
} menu_row_t;

static const char *const group_rows[APP_GROUP_COUNT] = {
    [APP_GROUP_SPECTRUM] = "RF tools >",
    [APP_GROUP_RECEIVE] = "Radios >",
    [APP_GROUP_RC] = "RC & drones >",
    [APP_GROUP_NFC] = "NFC >",
    [APP_GROUP_TRANSMIT] = "TX lab >",
    [APP_GROUP_TOOLS] = "Tools >",
};

static const char *const group_titles[APP_GROUP_COUNT] = {
    [APP_GROUP_SPECTRUM] = "RF TOOLS",
    [APP_GROUP_RECEIVE] = "RADIOS",
    [APP_GROUP_RC] = "RC + DRONES",
    [APP_GROUP_NFC] = "NFC",
    [APP_GROUP_TRANSMIT] = "TX LAB",
    [APP_GROUP_TOOLS] = "TOOLS",
    [APP_GROUP_SYSTEM] = "MENU",
};

static uint8_t m_top_sel;
static uint8_t m_group;
static uint8_t m_group_sel;

static bool group_has_entries(uint8_t group)
{
    for (uint8_t i = 0; i < g_app_screen_count; i++)
    {
        if (g_app_screens[i]->group == group)
            return true;
    }
    return false;
}

// The top level menu is the SYSTEM group plus a row for every other group
// that has something in it; an empty group is not shown
static uint8_t menu_build(uint8_t group, menu_row_t *rows)
{
    uint8_t n = 0;

    rows[n++] = (menu_row_t){.label = "Back", .group = APP_GROUP_HIDDEN};

    if (group == APP_GROUP_SYSTEM)
    {
        for (uint8_t g = APP_GROUP_SPECTRUM; g < APP_GROUP_SYSTEM; g++)
        {
            if (n < MENU_ROWS_MAX && group_has_entries(g))
                rows[n++] = (menu_row_t){.label = group_rows[g], .group = g};
        }
    }

    for (uint8_t i = 0; i < g_app_screen_count && n < MENU_ROWS_MAX; i++)
    {
        const app_screen_t *s = g_app_screens[i];
        if (s->group == group)
            rows[n++] = (menu_row_t){.label = s->name, .screen = s};
    }
    return n;
}

static void menu_tick(uint8_t group, uint8_t *sel)
{
    menu_row_t rows[MENU_ROWS_MAX];
    uint8_t n = menu_build(group, rows);

    if (*sel >= n)
        *sel = 0;

    if (app_left())
    {
        *sel = (uint8_t)((*sel + n - 1) % n);
        app_redraw();
    }
    if (app_right())
    {
        *sel = (uint8_t)((*sel + 1) % n);
        app_redraw();
    }

    if (app_ok())
    {
        const menu_row_t *row = &rows[*sel];
        if (row->screen && row->screen->tick)
        {
            app_open(row->screen);
        }
        else if (row->screen && row->screen->action)
        {
            // Actions stay in the menu, unless they navigate themselves
            row->screen->action();
            app_redraw();
        }
        else if (row->group != APP_GROUP_HIDDEN)
        {
            m_group = row->group;
            app_open(&scr_group);
        }
        else
        {
            app_back();
        }
        return;
    }

    if (app_take_redraw())
    {
        const char *labels[MENU_ROWS_MAX];
        const char *values[MENU_ROWS_MAX];
        for (uint8_t i = 0; i < n; i++)
        {
            labels[i] = rows[i].label;
            values[i] = (rows[i].screen && rows[i].screen->value) ? rows[i].screen->value() : 0;
        }
        ui_list(group_titles[group], labels, n, *sel, values);
    }
}

// ---------------------------------------------------------------------------
// Top level menu
// ---------------------------------------------------------------------------

static void top_menu_enter(void)
{
    m_top_sel = 0;
}

static void top_menu_tick(uint32_t now)
{
    menu_tick(APP_GROUP_SYSTEM, &m_top_sel);
}

const app_screen_t scr_menu = {
    .name = "Menu",
    .group = APP_GROUP_HIDDEN,
    .enter = top_menu_enter,
    .tick = top_menu_tick,
};

// ---------------------------------------------------------------------------
// Group submenu, m_group is set by the row that opens it
// ---------------------------------------------------------------------------

static void group_menu_enter(void)
{
    m_group_sel = 0;
}

static void group_menu_tick(uint32_t now)
{
    menu_tick(m_group, &m_group_sel);
}

const app_screen_t scr_group = {
    .name = "Group",
    .group = APP_GROUP_HIDDEN,
    .enter = group_menu_enter,
    .tick = group_menu_tick,
};

// ---------------------------------------------------------------------------
// Power actions on the top level menu
// ---------------------------------------------------------------------------

static void sleep_action(void)
{
    go_to_sleep("MENU");
}

const app_screen_t act_sleep = {
    .name = "Sleep",
    .group = APP_GROUP_SYSTEM,
    .action = sleep_action,
};

static void dfu_action(void)
{
    go_to_dfu(500);
}

const app_screen_t act_dfu = {
    .name = "DFU",
    .group = APP_GROUP_SYSTEM,
    .action = dfu_action,
};
