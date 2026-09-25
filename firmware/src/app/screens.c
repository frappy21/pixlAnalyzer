#include "screens.h"

// Every screen and action item, one line each, in menu order. The menu
// groups them by their .group field; APP_GROUP_SYSTEM entries are rows of the
// top level menu, APP_GROUP_HIDDEN ones never show up in a menu.
#define APP_SCREEN_LIST(X) \
    X(scr_scanner)         \
    X(scr_menu)            \
    X(scr_group)           \
    X(scr_intro)           \
    X(scr_top)             \
    X(scr_meter)           \
    X(act_overlay)         \
    X(act_freeze)          \
    X(act_set_ref)         \
    X(act_clear_max)       \
    X(act_view)            \
    X(act_trace)           \
    X(act_rbw)             \
    X(act_delta)           \
    X(act_cal)             \
    X(act_alarm)           \
    X(act_dwell)           \
    X(scr_identify)        \
    X(scr_ble)             \
    X(scr_ble_detail)      \
    X(scr_ble_hunt)        \
    X(scr_ble_sensors)     \
    X(act_ble_filter)      \
    X(act_ble_sort)        \
    X(act_ble_follow)      \
    X(act_ble_spam)        \
    X(act_ble_clear)       \
    X(scr_esb)             \
    X(scr_ant)             \
    X(scr_zigbee)          \
    X(scr_zigbee_pan)      \
    X(scr_sniff)           \
    X(scr_sniff_pkt)       \
    X(scr_rc)              \
    X(scr_radar)           \
    X(scr_nfc)             \
    X(scr_tx)              \
    X(scr_esb_tx)          \
    X(scr_esb_payload)     \
    X(scr_settings)        \
    X(scr_presets)         \
    X(scr_sentry)          \
    X(scr_info)            \
    X(scr_help)            \
    X(act_sleep)           \
    X(act_dfu)

// The main screens, in carousel order, grouped by category. The banner
// shows the category when it changes, the screen name otherwise. The first
// one is the default home screen; the settings can start the device on any
// of them (home_screen).
#define APP_HOME_LIST(X)            \
    /* RF tools */                   \
    X(scr_scanner, "SPECTRUM")       \
    X(scr_top, "WIFI")               \
    X(scr_meter, "METER")            \
    X(scr_identify, "IDENTIFY")      \
    /* radios */                     \
    X(scr_ble, "BLE")                \
    X(scr_zigbee, "ZIGBEE")          \
    X(scr_esb, "RC SNIFF")           \
    X(scr_ant, "ANT+")               \
    X(scr_sniff, "SNIFF")            \
    /* RC and drones */              \
    X(scr_rc, "RC DASH")             \
    X(scr_radar, "RADAR")            \
    /* NFC */                        \
    X(scr_nfc, "NFC")

#define APP_SCREEN_EXTERN(s) extern const app_screen_t s;
#define APP_SCREEN_ENTRY(s) &s,
#define APP_HOME_ENTRY(s, label) &s,
#define APP_HOME_LABEL(s, label) label,

APP_SCREEN_LIST(APP_SCREEN_EXTERN)

const app_screen_t *const g_app_screens[] = {APP_SCREEN_LIST(APP_SCREEN_ENTRY)};

const uint8_t g_app_screen_count = (uint8_t)(sizeof(g_app_screens) / sizeof(g_app_screens[0]));

const app_screen_t *const g_app_home[] = {APP_HOME_LIST(APP_HOME_ENTRY)};
const char *const g_app_home_label[] = {APP_HOME_LIST(APP_HOME_LABEL)};

const uint8_t g_app_home_count = (uint8_t)(sizeof(g_app_home) / sizeof(g_app_home[0]));
