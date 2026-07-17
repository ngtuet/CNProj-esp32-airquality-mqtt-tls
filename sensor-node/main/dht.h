/*
 * dht.h - bit-banged driver for DHT11 / DHT22 (AM2302) on ESP32.
 *
 * These sensors use a custom single-wire protocol (NOT Dallas 1-Wire).
 * Each sensor needs its own GPIO with a 4.7k-10k pull-up to 3.3V.
 * DHT11: >=1 s between reads. DHT22: >=2 s between reads.
 */
#ifndef DHT_H
#define DHT_H

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum { DHT_TYPE_DHT11 = 0, DHT_TYPE_DHT22 = 1 } dht_type_t;

// Reads one sample. Returns ESP_OK and fills *t_c, *rh on success.
// On failure returns an error and leaves outputs untouched.
esp_err_t dht_read(dht_type_t type, gpio_num_t pin, float *t_c, float *rh);

#endif // DHT_H
