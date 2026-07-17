/*
 * dht.c - bit-banged DHT11 / DHT22 driver.
 *
 * Protocol summary (40 bits total = 5 bytes):
 *   MCU pulls line low >=1ms (DHT11) / ~1-10ms (DHT22), releases high.
 *   Sensor responds: ~80us low, ~80us high.
 *   Then 40 bits: each bit = ~50us low + a high pulse whose LENGTH encodes
 *   the bit (short ~26-28us = '0', long ~70us = '1').
 *   5 bytes: RH_int, RH_dec, T_int, T_dec, checksum.
 *
 * DHT11 reports integer-only (dec bytes ~0); DHT22 packs 16-bit RH and T*10,
 * with the temperature sign in the top bit.
 *
 * Timing is sensitive: we disable interrupts during the bit capture window.
 */
#include "dht.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us
#include "esp_cpu.h"
#include "esp_log.h"

static const char *TAG = "dht";

// Wait until the pin reaches `level`, up to `timeout_us`. Returns elapsed us,
// or -1 on timeout.
static int wait_level(gpio_num_t pin, int level, int timeout_us) {
    int us = 0;
    while (gpio_get_level(pin) != level) {
        if (us++ > timeout_us) return -1;
        esp_rom_delay_us(1);
    }
    return us;
}

esp_err_t dht_read(dht_type_t type, gpio_num_t pin, float *t_c, float *rh) {
    uint8_t data[5] = {0};

    // --- Start signal ---
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    // DHT11 wants >=18ms low; DHT22 ~1-10ms. 20ms is safe for both.
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);

    // --- Switch to input, read response ---
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    // The bit capture is timing-critical: protect it from preemption/IRQs.
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);

    esp_err_t result = ESP_OK;

    // Sensor pulls low ~80us, then high ~80us (acknowledge)
    if (wait_level(pin, 0, 100) < 0) { result = ESP_ERR_TIMEOUT; goto done; }
    if (wait_level(pin, 1, 100) < 0) { result = ESP_ERR_TIMEOUT; goto done; }
    if (wait_level(pin, 0, 100) < 0) { result = ESP_ERR_TIMEOUT; goto done; }

    // Read 40 bits
    for (int i = 0; i < 40; i++) {
        // Each bit starts with ~50us low
        if (wait_level(pin, 1, 80) < 0) { result = ESP_ERR_TIMEOUT; goto done; }
        // Measure the high-pulse length
        int high_us = wait_level(pin, 0, 100);
        if (high_us < 0) { result = ESP_ERR_TIMEOUT; goto done; }
        // > ~40us high means '1'
        data[i / 8] <<= 1;
        if (high_us > 40) data[i / 8] |= 1;
    }

done:
    taskEXIT_CRITICAL(&mux);
    if (result != ESP_OK) return result;

    // --- Checksum ---
    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if (sum != data[4]) {
        ESP_LOGW(TAG, "checksum fail %02x%02x%02x%02x sum=%02x exp=%02x",
                 data[0], data[1], data[2], data[3], sum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    // --- Decode ---
    if (type == DHT_TYPE_DHT11) {
        *rh  = (float)data[0] + data[1] * 0.1f;     // dec byte usually 0
        *t_c = (float)data[2] + (data[3] & 0x7f) * 0.1f;
        if (data[3] & 0x80) *t_c = -*t_c;
    } else { // DHT22 / AM2302
        uint16_t rh_raw = (data[0] << 8) | data[1];
        uint16_t t_raw  = ((data[2] & 0x7f) << 8) | data[3];
        *rh  = rh_raw * 0.1f;
        *t_c = t_raw * 0.1f;
        if (data[2] & 0x80) *t_c = -*t_c;
    }

    // Sanity clamp - reject obviously bad frames
    if (*rh < 0 || *rh > 100 || *t_c < -40 || *t_c > 85)
        return ESP_ERR_INVALID_RESPONSE;

    return ESP_OK;
}
