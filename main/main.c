#include "esp_event.h"
#include "esp_http_server.h"
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
    wifi_config_t ap_config = { .ap = { .ssid = "C6-Zigbee-Gateway", .ssid_len = sizeof("C6-Zigbee-Gateway") - 1, .channel = 1, .password = "C6Gateway", .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi SoftAP started");
    ESP_LOGI(TAG, "SSID: C6-Zigbee-Gateway");
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");
}

static esp_err_t root_handler(httpd_req_t *req)
{
    static const char response[] = "{\"device\":\"C6-Zigbee-Gateway\",\"phase\":2,\"wifi\":\"softap\",\"ip\":\"192.168.4.1\",\"zigbee\":\"not_started\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_LOGI(TAG, "HTTP server started at http://192.168.4.1/");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee gateway starting");
    ESP_LOGI(TAG, "Phase 2: Wi-Fi SoftAP + HTTP status");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);
    start_wifi_ap();
    start_http_server();
}
