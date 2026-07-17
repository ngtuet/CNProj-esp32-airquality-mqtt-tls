/*
 * sensors.h - SHT3x + ADS1115 over a shared I2C master bus.
 */
#ifndef SENSORS_H
#define SENSORS_H

#include "esp_err.h"

// Pin / bus config
#define I2C_PORT          0
#define I2C_SDA           21
#define I2C_SCL           22
#define I2C_FREQ_HZ       100000
#define SHT3X_ADDR        0x44
#define ADS1115_ADDR      0x48

// ADS1115 differential mux selections
#define ADS_MUX_DIFF_01   0b000   // AIN0 - AIN1  (CO)
#define ADS_MUX_DIFF_23   0b011   // AIN2 - AIN3  (NO2)

// PGA = +/-0.512 V full scale
#define ADS_PGA           0b100   // +/-0.512 V FS
#define ADS_FS_MV         512.0f
#define ADS_AVG_N         16
#define ADS_INTER_MS      5

esp_err_t sensors_init(void);
esp_err_t sht3x_read(float *t_c, float *rh);     // NaN on failure handled by caller
float     ads_diff_mv(uint8_t mux);              // returns NaN on total failure

#endif // SENSORS_H
