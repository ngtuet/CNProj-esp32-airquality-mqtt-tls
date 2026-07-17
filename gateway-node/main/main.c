/*
 * GATEWAY NODE  (Board B)  - VGU Communication Networks Lab
 *
 * ESP32-WROOM-32. No sensors attached.
 * Reads JSON lines from the sensor node over UART2 (GPIO16 = RX) and
 * republishes each line to MQTT (TLS optional). The payload is forwarded
 * as-is, so the dashboard sees the same JSON the sensor node produced.
 *
 * Inter-board link:
 *   Sensor GPIO17 (TX2) ---> Gateway GPIO16 (RX2)
 *   Sensor GND          <--> Gateway GND
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"

// ---- EDIT THESE ----
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASS"
#define MQTT_URI    "mqtts://YOUR_BROKER_IP:8884"  
#define MQTT_TOPIC  "YOUR_MQTT_TOPIC"
#define USE_TLS     1
#define MQTT_USER   "YOUR_MQTT_USER"
#define MQTT_PASS   "YOUR_MQTT_PASS"
// --------------------

// ---- Inter-board UART link (UART2) ----
#define LINK_UART   UART_NUM_2
#define LINK_TX_PIN 17     // unused for one-way receive, wired for future use
#define LINK_RX_PIN 16
#define LINK_BAUD   115200

#if USE_TLS
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[]   asm("_binary_ca_crt_end");
#endif

static const char *TAG = "gateway";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = {0};
    strncpy((char*)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "WiFi connecting to %s", WIFI_SSID);
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *data) {
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;  ESP_LOGI(TAG, "MQTT connected"); break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false; ESP_LOGW(TAG, "MQTT disconnected"); break;
        default: break;
    }
}

static void mqtt_init(void) {
    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.uri = MQTT_URI;
	mc.credentials.username = MQTT_USER;
    mc.credentials.authentication.password = MQTT_PASS;
#if USE_TLS
    mc.broker.verification.certificate = (const char *)ca_crt_start;
#endif
    mqtt_client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
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
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    link_uart_init();
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(4000));
    mqtt_init();

    ESP_LOGI(TAG, "gateway up - forwarding UART2 -> MQTT %s", MQTT_TOPIC);

    char line[256];
    int idx = 0;
    uint8_t ch;

    while (1) {
        // Read bytes from the sensor node, assemble lines on '\n'
        int n = uart_read_bytes(LINK_UART, &ch, 1, pdMS_TO_TICKS(1000));
        if (n == 1) {
            if (ch == '\n') {
                if (idx > 0) {
                    line[idx] = 0;
                    // Forward the JSON line as the MQTT payload
                    if (mqtt_connected) {
                        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC,
                                                line, idx, 0, 0);
                    }
                    ESP_LOGI(TAG, "fwd: %s", line);
                    idx = 0;
                }
            } else if (ch != '\r' && idx < (int)sizeof(line) - 1) {
                line[idx++] = ch;
            }
        }
    }
}
