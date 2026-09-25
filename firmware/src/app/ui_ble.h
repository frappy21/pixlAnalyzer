/** BLE scan rendering: device list, device detail, hunt view, sensor dashboard. */
#ifndef PIXLA_UI_BLE_H
#define PIXLA_UI_BLE_H

#include <stdint.h>

#include "ble_scan.h"
#include "ble_sensor.h"

typedef struct
{
    const uint8_t *idx;             // table indices, filtered and sorted
    uint8_t n;
    uint8_t selected;
    uint8_t filter;                 // ble_filter_t
    uint32_t packets;
    uint8_t following;              // devices the follow alert fires on
    const ble_spam_state_t *spam;   // NULL when the spam alert is off
    const ble_ext_stats_t *ext;
} ble_list_view_t;

void ui_ble_list(const ble_list_view_t *view);

// Detail card: the device's log entry and every decoded AD structure, as
// text lines. Build once per redraw, then draw from the given first line.
uint8_t ui_ble_detail_lines(const ble_dev_t *dev, uint32_t now_ms, char (*lines)[BLE_LINE_LEN],
                            uint8_t max);
void ui_ble_detail(char (*lines)[BLE_LINE_LEN], uint8_t count, uint8_t first);
#define UI_BLE_DETAIL_ROWS 7

// Hunt view: level, bar and trend of one device. rssi 0 = not heard lately.
// trend holds RSSI samples (0 = not heard), oldest first.
void ui_ble_hunt(const ble_dev_t *dev, int8_t rssi, uint32_t age_ms, const int8_t *trend,
                 uint8_t trend_len);

// Sensor dashboard
typedef struct
{
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t packets;
    uint32_t last_ms;
    ble_sensor_t s;
    char name[10];
} ble_sensor_entry_t;

void ui_ble_sensors(const ble_sensor_entry_t *list, uint8_t n, uint8_t selected, uint32_t now_ms);

#endif // PIXLA_UI_BLE_H
