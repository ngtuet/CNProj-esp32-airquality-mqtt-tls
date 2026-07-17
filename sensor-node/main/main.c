/*
 * SENSOR NODE  (Board A)  - VGU Communication Networks Lab
 *
 * ESP32-WROOM-32 + SHT3x + ADS1115 + Alphasense CO-B4 / NO2-B43F (ISB)
 *
 * Reads all sensors, applies the calibration, and sends ONE JSON line every
 * SAMPLE_PERIOD_MS over UART2 (GPIO17 = TX) to the gateway node.
 * Also prints the same line on the USB console (UART0) for debugging.
 *
 * This board does NOT use WiFi - keeps the analog supply clean.
 *
 * Inter-board link:
 *   Sensor GPIO17 (TX2) ---> Gateway GPIO16 (RX2)
 *   Sensor GND          <--> Gateway GND        (common ground, required)
 *
 * ppb = (mv - zero_mv) / sensitivity_mv_per_ppb
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"

#include "sensors.h"
#include "dht.h"

// ---- Calibration: published Alphasense defaults; refine via S500 ----
// CO-B4    sensitivity 420..650 nA/ppm ; NO2-B43F -200..-650 nA/ppm
#define CAL_CO_SENS    0.350f     // published nominal
#define CAL_CO_ZERO    131.0f     // current settled clean-air baseline
#define CAL_NO2_SENS  -0.250f     // published nominal (no slope data to refine)
#define CAL_NO2_ZERO   3.80f      // anchored to S500 = 40 ppb on 2026-06-29
#define GAS_MEDIAN_N  7

// ---- DHT comparison sensors (each on its own GPIO, NOT the I2C bus) ----
// Both need a 4.7k-10k pull-up on the data line to 3.3V (modules often
// include it). Power at 3.3V so the data line is 3.3V logic.
#define DHT22_PIN      25      // GPIO25
#define DHT11_PIN      26      // GPIO26

// ---- Inter-board UART link (UART2) ----
#define LINK_UART      UART_NUM_2
#define LINK_TX_PIN    17
#define LINK_RX_PIN    16     // unused for one-way, wired for future use
#define LINK_BAUD      115200

#define SAMPLE_PERIOD_MS  1000

// ---- Gas filtering ----
// Median filter removes short spikes better than a simple moving average.
// GAS_MEDIAN_N must be odd: 3, 5, 7, 9...
// 7 samples at ~1.4 s/sample gives roughly 10 s of smoothing.
#define GAS_MEDIAN_N  7

// Clamp negative gas concentration to 0 ppb.
// Negative ppb is not physical; it only means baseline/noise drift.
#define CLAMP_NEGATIVE_GAS_TO_ZERO  1

static const char *TAG = "sensor_node";

typedef struct {
    float buf[GAS_MEDIAN_N];
    int count;
    int idx;
} median_filter_t;

static median_filter_t co_filter = {0};
static median_filter_t no2_filter = {0};

static float median_push(median_filter_t *f, float x) {
    if (isnan(x)) {
        return NAN;
    }

    f->buf[f->idx] = x;
    f->idx = (f->idx + 1) % GAS_MEDIAN_N;

    if (f->count < GAS_MEDIAN_N) {
        f->count++;
    }

    float tmp[GAS_MEDIAN_N];
    for (int i = 0; i < f->count; i++) {
        tmp[i] = f->buf[i];
    }

    // Small insertion sort
    for (int i = 1; i < f->count; i++) {
        float key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    return tmp[f->count / 2];
}

static void link_uart_init(void) {
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void) {
    if (sensors_init() != ESP_OK)
        ESP_LOGE(TAG, "sensor init failed - check wiring/I2C");

    link_uart_init();

    // Console (debug) header
    printf("# SENSOR NODE - sends JSON over UART2 @ %d baud\n", LINK_BAUD);
    printf("# CALIB co_sens=%.4f co_zero=%.4f no2_sens=%.4f no2_zero=%.4f\n",
       CAL_CO_SENS, CAL_CO_ZERO, CAL_NO2_SENS, CAL_NO2_ZERO);
	printf("# GAS FILTER: median window = %d samples, negative gas clamp = %s\n",
       GAS_MEDIAN_N, CLAMP_NEGATIVE_GAS_TO_ZERO ? "on" : "off");
	printf("# legend: gas ppb values are computed from median-filtered ADS1115 mV\n");
	printf("# columns: time | SHT3x | DHT22 | DHT11 | CO | NO2\n");
    int64_t t0 = esp_timer_get_time();

    while (1) {
        float t_s = (esp_timer_get_time() - t0) / 1e6f;
        float t_c = NAN, rh = NAN;
        if (sht3x_read(&t_c, &rh) != ESP_OK) { t_c = NAN; rh = NAN; }

        // Read raw differential voltages from ADS1115
	float co_mv_raw  = ads_diff_mv(ADS_MUX_DIFF_01);
	float no2_mv_raw = ads_diff_mv(ADS_MUX_DIFF_23);

// Median-filter the gas voltages before converting to ppb.
// Filtering at the mV level is cleaner because the voltage is the physical measurement.
	float co_mv  = median_push(&co_filter, co_mv_raw);	
	float no2_mv = median_push(&no2_filter, no2_mv_raw);

// Convert filtered mV to ppb using calibration constants
	float co_ppb  = (co_mv  - CAL_CO_ZERO)  / CAL_CO_SENS;
	float no2_ppb = (no2_mv - CAL_NO2_ZERO) / CAL_NO2_SENS;

#if CLAMP_NEGATIVE_GAS_TO_ZERO
	if (!isnan(co_ppb) && co_ppb < 0.0f) {
		co_ppb = 0.0f;
}
	if (!isnan(no2_ppb) && no2_ppb < 0.0f) {
		no2_ppb = 0.0f;
}
#endif

        // Comparison temp/humidity sensors. NaN if a read fails this cycle
        // (DHT reads occasionally drop a frame - that is normal).
        float d22_t = NAN, d22_h = NAN, d11_t = NAN, d11_h = NAN;
        bool d22_ok = (dht_read(DHT_TYPE_DHT22, DHT22_PIN, &d22_t, &d22_h) == ESP_OK);
        bool d11_ok = (dht_read(DHT_TYPE_DHT11, DHT11_PIN, &d11_t, &d11_h) == ESP_OK);
        bool sht_ok = !isnan(t_c);

        // Build JSON. A failed read must emit JSON `null`, NOT "nan"
        // (printf would print "nan", which breaks the JSON parser).
        // We assemble each numeric-or-null field with a small helper macro.
        char co_mv_s[24], no2_mv_s[24];
        char sht_t_s[16], sht_h_s[16];
        char d22_t_s[16], d22_h_s[16], d11_t_s[16], d11_h_s[16];
        snprintf(co_mv_s,  sizeof(co_mv_s),  "%.4f", co_mv);
        snprintf(no2_mv_s, sizeof(no2_mv_s), "%.4f", no2_mv);
        #define FNUM(buf, ok, val, fmt) \
            do { if (ok) snprintf(buf, sizeof(buf), fmt, val); \
                 else    snprintf(buf, sizeof(buf), "null"); } while (0)
        FNUM(sht_t_s, sht_ok, t_c,   "%.2f");
        FNUM(sht_h_s, sht_ok, rh,    "%.1f");
        FNUM(d22_t_s, d22_ok, d22_t, "%.1f");
        FNUM(d22_h_s, d22_ok, d22_h, "%.1f");
        FNUM(d11_t_s, d11_ok, d11_t, "%.1f");
        FNUM(d11_h_s, d11_ok, d11_h, "%.1f");
        #undef FNUM

        char line[320];
        int len = snprintf(line, sizeof(line),
            "{\"node\":\"node1\",\"t_s\":%.1f,"
            "\"sht_temp_c\":%s,\"sht_rh_pct\":%s,"
            "\"dht22_temp_c\":%s,\"dht22_rh_pct\":%s,"
            "\"dht11_temp_c\":%s,\"dht11_rh_pct\":%s,"
            "\"co_mv\":%s,\"no2_mv\":%s,"
            "\"co_ppb\":%.1f,\"no2_ppb\":%.1f}\n",
            t_s, sht_t_s, sht_h_s, d22_t_s, d22_h_s, d11_t_s, d11_h_s,
            co_mv_s, no2_mv_s, co_ppb, no2_ppb);

        // Send to gateway over UART2
        // Send the JSON to the gateway over UART2 (machine-readable, do not change)
        uart_write_bytes(LINK_UART, line, len);

        // Human-readable console line (column-aligned). The "%s" entries reuse
        // the same string buffers built above, which already contain either a
        // number or "null", so failed reads show as null here too.
        printf("t=%6.1fs | SHT %6s C %5s%% | DHT22 %6s C %5s%% | "
       "DHT11 %6s C %5s%% | CO med %8s mV (%6.1f ppb) | "
       "NO2 med %8s mV (%6.1f ppb)\n",
       t_s, sht_t_s, sht_h_s, d22_t_s, d22_h_s,
       d11_t_s, d11_h_s, co_mv_s, co_ppb, no2_mv_s, no2_ppb);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}