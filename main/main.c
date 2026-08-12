#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "c6_gateway";

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "C6-Zigbee-Gateway",
            .ssid_len = sizeof("C6-Zigbee-Gateway") - 1,
            .channel = 1,
            .password = "C6Gateway",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi SoftAP started");
    ESP_LOGI(TAG, "SSID: C6-Zigbee-Gateway");
    ESP_LOGI(TAG, "Password: C6Gateway");
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");
    ESP_LOGI(TAG, "Maximum clients: 4");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee gateway starting");
    ESP_LOGI(TAG, "Phase 1: Wi-Fi SoftAP");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    start_wifi_ap();
}
