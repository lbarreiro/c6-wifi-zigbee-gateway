#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "c6_gateway";
static volatile bool zigbee_started = false;

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

static esp_err_t status_handler(httpd_req_t *req)
{
    const char *zigbee = zigbee_started ? "running" : "starting";
    char response[192];
    snprintf(response, sizeof(response),
             "{\"device\":\"C6-Zigbee-Gateway\",\"phase\":3,\"wifi\":\"softap\",\"ip\":\"192.168.4.1\",\"zigbee\":\"%s\",\"zigbee_role\":\"router\"}",
             zigbee);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t join_handler(httpd_req_t *req)
{
    if (!zigbee_started) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Zigbee stack is not ready");
    }

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();

    if (err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Zigbee network steering failed to start: %d", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to start Zigbee network steering");
    }

    ESP_LOGI(TAG, "Zigbee network steering started from HTTP");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req,
        "<html><head><meta name=\"viewport\" content=\"width=device-width\"><title>C6 Zigbee Gateway</title></head>"
        "<body style=\"font-family:sans-serif;max-width:600px;margin:40px auto;padding:20px\">"
        "<h1>C6 Wi-Fi + Zigbee Gateway</h1>"
        "<p><b>Zigbee network steering started.</b></p>"
        "<p>The C6 is now scanning for an existing Zigbee network.</p>"
        "<p><a href=\"/\">Back to status</a></p></body></html>");
}

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *joined = "unknown";
    esp_zigbee_lock_acquire(portMAX_DELAY);
    joined = ezb_bdb_dev_joined() ? "yes" : "no";
    esp_zigbee_lock_release();

    httpd_resp_set_type(req, "text/html");
    char response[1800];
    snprintf(response, sizeof(response),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>C6 Zigbee Gateway</title></head>"
        "<body style=\"font-family:-apple-system,BlinkMacSystemFont,sans-serif;max-width:600px;margin:30px auto;padding:20px\">"
        "<h1>C6 Wi-Fi + Zigbee Gateway</h1>"
        "<p><b>Wi-Fi:</b> SoftAP — 192.168.4.1</p>"
        "<p><b>Zigbee stack:</b> %s</p>"
        "<p><b>Zigbee role:</b> Router</p>"
        "<p><b>Joined Zigbee network:</b> %s</p>"
        "<p><a href=\"/join\" style=\"display:inline-block;padding:14px 20px;background:#007aff;color:white;text-decoration:none;border-radius:10px\">Join Zigbee Network</a></p>"
        "<p><a href=\"/api/status\">View JSON status</a></p>"
        "</body></html>",
        zigbee_started ? "running" : "starting", joined);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    const httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    const httpd_uri_t join = { .uri = "/join", .method = HTTP_GET, .handler = join_handler, .user_ctx = NULL };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &join));
    ESP_LOGI(TAG, "HTTP server started at http://192.168.4.1/");
}

static void zigbee_main_task(void *arg)
{
    ESP_LOGI(TAG, "Phase 3: Zigbee stack initialization");
    esp_zigbee_config_t config = {
        .platform_config = {
            .storage_partition_name = "zb_storage",
            .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = { .max_children = 10 },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_LOGI(TAG, "Zigbee stack initialized");
    ESP_LOGI(TAG, "Zigbee role: Router");
    ESP_LOGI(TAG, "Zigbee SDK: %s", esp_zigbee_get_version_string());

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    zigbee_started = true;
    ESP_LOGI(TAG, "Zigbee stack started");
    ESP_LOGI(TAG, "Zigbee network join/commissioning is waiting for HTTP command");

    esp_zigbee_launch_mainloop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee gateway starting");
    ESP_LOGI(TAG, "Phase 4: Wi-Fi SoftAP + Zigbee network steering");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    start_wifi_ap();
    start_http_server();
    xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL);
}
