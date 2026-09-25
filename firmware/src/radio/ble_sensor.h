/**
 * Decoders for the BLE sensor adverts people have around the house:
 * temperature, humidity and battery from
 *
 *  - BTHome v2 (service data 0xFCD2), unencrypted only
 *  - ATC1441 and pvvx custom firmware for the Xiaomi thermometers (0x181A)
 *  - Xiaomi MiBeacon (0xFE95), unencrypted objects only
 *  - Govee H5074 / H5075 style manufacturer data (0xEC88)
 *  - Ruuvi RAWv2 (company 0x0499, data format 5)
 *  - Inkbird IBS-TH1 / TH2 ("sps" / "tps")
 *  - SwitchBot meters (service data 0xFD3D, manufacturer data 0x0969)
 *
 * Pure functions, host tested. An encrypted payload is reported as such and
 * never decrypted.
 */
#ifndef PIXLA_BLE_SENSOR_H
#define PIXLA_BLE_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BLE_SENSOR_NONE = 0,
    BLE_SENSOR_BTHOME,
    BLE_SENSOR_ATC,
    BLE_SENSOR_PVVX,
    BLE_SENSOR_MIBEACON,
    BLE_SENSOR_GOVEE,
    BLE_SENSOR_RUUVI,
    BLE_SENSOR_INKBIRD,
    BLE_SENSOR_SWITCHBOT,
    BLE_SENSOR_COUNT
} ble_sensor_fmt_t;

// Which fields hold a value
#define BLE_SENSOR_TEMP 0x01
#define BLE_SENSOR_HUM 0x02
#define BLE_SENSOR_BATT 0x04
#define BLE_SENSOR_MV 0x08
#define BLE_SENSOR_ENCRYPTED 0x10

typedef struct
{
    uint8_t fmt;     // ble_sensor_fmt_t
    uint8_t valid;   // BLE_SENSOR_* bits
    int16_t temp;    // centi degrees C
    uint16_t hum;    // centi percent
    uint8_t batt;    // percent
    uint16_t mv;     // battery millivolts
} ble_sensor_t;

// Tries every format on one advert. Returns true when it is a sensor advert,
// including an encrypted one (valid == BLE_SENSOR_ENCRYPTED).
bool ble_sensor_decode(const uint8_t *ad, uint8_t len, ble_sensor_t *out);

const char *ble_sensor_name(uint8_t fmt);

#endif // PIXLA_BLE_SENSOR_H
