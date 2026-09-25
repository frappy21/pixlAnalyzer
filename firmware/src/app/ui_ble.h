/** BLE scan rendering: device list and device detail. */
#ifndef PIXLA_UI_BLE_H
#define PIXLA_UI_BLE_H

#include <stdint.h>

#include "ble_scan.h"

void ui_ble_list(uint8_t selected, uint32_t packets);
void ui_ble_detail(const ble_dev_t *dev);

#endif // PIXLA_UI_BLE_H
