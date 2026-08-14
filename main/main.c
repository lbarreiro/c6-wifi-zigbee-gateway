#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/bdb.h"
#include "ezbee/app_signals.h"
#include "ezbee/nwk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "c6_gateway";
#define STATUS_INTERVAL_MS 5000
#define ZIGBEE_CHANNEL_MASK 0x07FFF800UL
#define WIFI_SSID "LB IN"
#define WIFI_PASSWORD "Beatriz77"
#define WIFI_CONNECTED_BIT BIT0

typedef enum {
    ZIGBEE_STATE_NOT_JOINED = 0,
    ZIGBEE_STATE_RESTORED,
    ZIGBEE_STATE_JOINED,
} zigbee_state_t;

static EventGroupHandle_t wifi_events;
static volatile zigbee_state_t zigbee_state = ZIGBEE_STATE_NOT_JOINED;
static volatile bool wifi_connected = false;
static esp_netif_t *wifi_netif = NULL;

static const char *zigbee_state_name(zigbee_state_t state)
{
    switch (state) {
        case ZIGBEE_STATE_JOINED:   return "JOINED";
        case ZIGBEE_STATE_RESTORED: return "RESTORED";
        default:                     return "NOT JOINED";
    }
}

static void start_network_steering(void)
{
    ESP_LOGI(TAG, "Starting Zigbee network steering");
    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
}

static void zigbee_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    start_network_steering();
    vTaskDelete(NULL);
}

static bool zigbee_signal_handler(const ezb_app_signal_t *signal)
{
    if (!signal) return false;

    switch (ezb_app_signal_get_type(signal)) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee ready; starting BDB initialization");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const ezb_bdb_comm_status_t status =
                *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            const bool factory_new = ezb_bdb_is_factory_new();
            ESP_LOGI(TAG, "Zigbee startup: status=0x%02x factory_new=%s",
                     status, factory_new ? "yes" : "no");
            if (status == EZB_BDB_STATUS_SUCCESS && factory_new) {
                zigbee_state = ZIGBEE_STATE_NOT_JOINED;
                start_network_steering();
            } else if (status == EZB_BDB_STATUS_SUCCESS) {
                /* Persisted network credentials mean the previous network was
                   restored, but do not claim a fresh JOIN until the Zigbee
                   stack reports a successful steering/rejoin event. */
                zigbee_state = ZIGBEE_STATE_RESTORED;
                ESP_LOGI(TAG, "Existing Zigbee network restored; join status not assumed");
            }
            break;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            const ezb_bdb_comm_status_t status =
                *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                zigbee_state = ZIGBEE_STATE_JOINED;
                ESP_LOGI(TAG, "JOINED Zigbee network | PAN=0x%04hx Channel=%d ShortAddr=0x%04hx",
                         ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            } else {
                zigbee_state = ZIGBEE_STATE_NOT_JOINED;
                ESP_LOGW(TAG, "Network steering failed: status=0x%02x", status);
                if (xTaskCreate(zigbee_retry_task, "zb_retry", 3072, NULL, 4, NULL) != pdPASS) {
                    ESP_LOGE(TAG, "Failed to schedule Zigbee steering retry");
                }
            }
            break;
        }
        default:
            break;
    }
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
            ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_connected = true;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected | IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_netif = esp_netif_create_default_wifi_sta();
    if (!wifi_netif) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi STA netif");
        abort();
    }

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
    ESP_LOGI(TAG, "Wi-Fi STA started; connecting to %s", WIFI_SSID);
}

static void zigbee_init(void)
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

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZIGBEE_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ZIGBEE_CHANNEL_MASK));
    ezb_aps_secur_enable_distributed_security(false);
    ezb_nwk_set_min_join_lqi(32);
    ESP_LOGI(TAG, "Zigbee BDB configured for channels 11-26");
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(zigbee_signal_handler));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_LOGI(TAG, "Zigbee stack started; joining network");
}

static void zigbee_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_launch_mainloop();
    ESP_LOGE(TAG, "Zigbee mainloop returned");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee coexistence test starting");
    ESP_LOGI(TAG, "Both radios remain active; status sampled every 5 seconds");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_flash_init_partition("zb_storage"));

    wifi_init_sta();
    zigbee_init();

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }

    for (uint32_t check = 1;; ++check) {
        int8_t rssi = 0;
        wifi_ap_record_t ap = {0};
        if (wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "[%03lu] Wi-Fi: %s | RSSI: %d dBm | Zigbee: %s",
                 (unsigned long)check,
                 wifi_connected ? "CONNECTED" : "DISCONNECTED",
                 (int)rssi,
                 zigbee_state_name(zigbee_state));
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}
