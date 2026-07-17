#include "sensors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "sensors";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t sht_dev, ads_dev;

#define ADS_REG_CONV    0x00
#define ADS_REG_CONFIG  0x01

static uint8_t sht_crc(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

esp_err_t sensors_init(void) {
    i2c_master_bus_config_t bcfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_PORT,
        .scl_io_num        = I2C_SCL,
        .sda_io_num        = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bcfg, &bus);
    if (e != ESP_OK) return e;

    i2c_device_config_t sht_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHT3X_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    e = i2c_master_bus_add_device(bus, &sht_cfg, &sht_dev);
    if (e != ESP_OK) return e;

    i2c_device_config_t ads_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &ads_cfg, &ads_dev);
}

esp_err_t sht3x_read(float *t_c, float *rh) {
    uint8_t cmd[2] = {0x2C, 0x06};
    esp_err_t r = i2c_master_transmit(sht_dev, cmd, 2, 100);
    if (r != ESP_OK) return r;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t buf[6];
    r = i2c_master_receive(sht_dev, buf, 6, 100);
    if (r != ESP_OK) return r;
    if (sht_crc(buf, 2) != buf[2] || sht_crc(buf+3, 2) != buf[5])
        return ESP_ERR_INVALID_CRC;
    uint16_t rawT = (buf[0] << 8) | buf[1];
    uint16_t rawH = (buf[3] << 8) | buf[4];
    *t_c = -45.0f + 175.0f * rawT / 65535.0f;
    *rh  = 100.0f * rawH / 65535.0f;
    return ESP_OK;
}

static esp_err_t ads_read_diff_raw(uint8_t mux, int16_t *raw) {
    uint16_t cfg = 0;
    cfg |= (1 << 15);
    cfg |= (mux & 0x07) << 12;
    cfg |= (ADS_PGA & 0x07) << 9;
    cfg |= (1 << 8);
    cfg |= (0x04 << 5);   // 128 SPS
    cfg |= 0x03;
    uint8_t w[3] = {ADS_REG_CONFIG, cfg >> 8, cfg & 0xFF};
    esp_err_t e = i2c_master_transmit(ads_dev, w, 3, 100);
    if (e != ESP_OK) return e;
    vTaskDelay(pdMS_TO_TICKS(12));
    uint8_t reg = ADS_REG_CONV;
    uint8_t r[2];
    e = i2c_master_transmit(ads_dev, &reg, 1, 100);
    if (e != ESP_OK) return e;
    e = i2c_master_receive(ads_dev, r, 2, 100);
    if (e != ESP_OK) return e;
    *raw = (int16_t)((r[0] << 8) | r[1]);
    return ESP_OK;
}

float ads_diff_mv(uint8_t mux) {
    int64_t acc = 0; int n_ok = 0;
    for (int i = 0; i < ADS_AVG_N; i++) {
        int16_t raw;
        if (ads_read_diff_raw(mux, &raw) == ESP_OK) { acc += raw; n_ok++; }
        if (ADS_INTER_MS > 0) vTaskDelay(pdMS_TO_TICKS(ADS_INTER_MS));
    }
    if (n_ok == 0) return 0.0f / 0.0f;
    float avg_raw = (float)acc / (float)n_ok;
    return (avg_raw * ADS_FS_MV) / 32768.0f;
}
