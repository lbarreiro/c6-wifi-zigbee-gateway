#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "c6_gateway";

#define WIFI_SSID "LB IN"
#define WIFI_PASSWORD "Beatriz77"
#define STATUS_INTERVAL_MS 5000

static esp_netif_t *sta_netif = NULL;
static EventGroupHandle_t wifi_event_group;
static const EventBits_t WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_wifi_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA started");
    ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
    return ESP_OK;
}

static esp_err_t init_zigbee_storage(void)
{
    esp_err_t err = nvs_flash_init_partition("zb_storage");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase_partition("zb_storage");
        if (err == ESP_OK) err = nvs_flash_init_partition("zb_storage");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee NVS init failed: %s (0x%x)", esp_err_to_name(err), err);
    }
    return err;
}

static void zigbee_main_task(void *arg)
{
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

    ESP_LOGI(TAG, "Zigbee initializing as Router");

    if (init_zigbee_storage() != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee storage initialization failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_zigbee_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s (0x%x)", esp_err_to_name(err), err);
        vTaskDelete(NULL);
        return;
    }

    /* Autostart runs the normal Zigbee startup/BDB procedure. For a factory-new
       Router this allows network steering; if persistent network data exists,
       the stack can rejoin that network. */
    err = esp_zigbee_start(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s (0x%x)", esp_err_to_name(err), err);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Zigbee stack started; waiting for network association");
    esp_zigbee_launch_mainloop();
    ESP_LOGE(TAG, "Zigbee mainloop returned");
    vTaskDelete(NULL);
}

static void status_task(void *arg)
{
    uint32_t check = 0;

    while (true) {
        ++check;

        bool wifi_connected = (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
        bool zigbee_joined = ezb_bdb_dev_joined();

        esp_netif_ip_info_t ip_info = {0};
        if (sta_netif != NULL) {
            esp_netif_get_ip_info(sta_netif, &ip_info);
        }

        ESP_LOGI(TAG,
                 "[%03lu] Wi-Fi: %s | IP: " IPSTR " | Zigbee: %s",
                 (unsigned long)check,
                 wifi_connected ? "CONNECTED" : "DISCONNECTED",
                 IP2STR(&ip_info.ip),
                 zigbee_joined ? "JOINED" : "NOT JOINED");

        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee coexistence test starting");
    ESP_LOGI(TAG, "Wi-Fi STA + Zigbee Router remain active simultaneously");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (init_wifi_sta() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
        return;
    }

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }

    if (xTaskCreate(status_task, "status", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status task");
        return;
    }
}
