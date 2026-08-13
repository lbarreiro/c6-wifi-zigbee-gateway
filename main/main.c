#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "ezbee/app_signals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_gateway";
#define STATUS_INTERVAL_MS 5000
static volatile bool zigbee_joined = false;

static bool zigbee_app_signal_handler(const ezb_app_signal_t *signal)
{
    if (!signal) return false;
    const ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(signal);

    switch (signal_type) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee ready; starting BDB initialization");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            ESP_LOGI(TAG, "Zigbee startup: status=0x%02x factory_new=%s", status,
                     ezb_bdb_is_factory_new() ? "yes" : "no");
            if (status == EZB_BDB_STATUS_SUCCESS && ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting network steering; open Permit Join in Zigbee2MQTT");
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else if (status == EZB_BDB_STATUS_SUCCESS) {
                zigbee_joined = true;
                ESP_LOGI(TAG, "Existing Zigbee network restored");
            }
            break;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                zigbee_joined = true;
                ESP_LOGI(TAG, "JOINED Zigbee network");
                ESP_LOGI(TAG, "PAN=0x%04hx Channel=%d ShortAddr=0x%04hx",
                         ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            } else {
                zigbee_joined = false;
                ESP_LOGW(TAG, "Network steering failed: status=0x%02x; retrying", status);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
            break;
        }
        default:
            break;
    }
    return true;
}

static void zigbee_main_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Zigbee-only connectivity test");
    ESP_LOGI(TAG, "Router -> Zigbee2MQTT");

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

    ESP_LOGI(TAG, "Initializing Zigbee stack");
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(zigbee_app_signal_handler));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_LOGI(TAG, "Zigbee stack started; waiting for Zigbee2MQTT Permit Join");
    esp_zigbee_launch_mainloop();
    ESP_LOGE(TAG, "Zigbee mainloop returned");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Zigbee-only test starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_flash_init_partition("zb_storage"));

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }
    for (uint32_t check = 1;; ++check) {
        ESP_LOGI(TAG, "[%03lu] Zigbee: %s", (unsigned long)check,
                 zigbee_joined ? "JOINED" : "NOT JOINED");
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}
