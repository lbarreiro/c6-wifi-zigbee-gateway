#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "ezbee/app_signals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_gateway";
static volatile bool joined = false;

static bool zigbee_signal_handler(const ezb_app_signal_t *signal)
{
    if (!signal) return false;

    switch (ezb_app_signal_get_type(signal)) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee ready; starting initialization");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const ezb_bdb_comm_status_t status =
                *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            ESP_LOGI(TAG, "BDB startup status=0x%02x", status);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Starting Zigbee network steering");
                ESP_ERROR_CHECK(ezb_bdb_start_top_level_commissioning(
                    EZB_BDB_MODE_NETWORK_STEERING));
            }
            break;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            const ezb_bdb_comm_status_t status =
                *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                joined = true;
                ESP_LOGI(TAG, "JOINED Zigbee network");
            } else {
                joined = false;
                ESP_LOGW(TAG, "Zigbee steering failed: status=0x%02x; retrying", status);
                ESP_ERROR_CHECK(ezb_bdb_start_top_level_commissioning(
                    EZB_BDB_MODE_NETWORK_STEERING));
            }
            break;
        }

        default:
            break;
    }
    return true;
}

static void zigbee_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "ESP32-C6 Zigbee-only connectivity test");
    ESP_LOGI(TAG, "Router -> any compatible coordinator");

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

    /* Follow the Espressif BDB examples: explicitly allow the full
       Zigbee primary/secondary channel range (11-26) for discovery. */
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(0x07FFF800));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0x07FFF800));
    ESP_LOGI(TAG, "BDB channel masks: primary=0x%08lx secondary=0x%08lx",
             (unsigned long)ezb_bdb_get_primary_channel_set(),
             (unsigned long)ezb_bdb_get_secondary_channel_set());

    ESP_ERROR_CHECK(ezb_app_signal_add_handler(zigbee_signal_handler));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_LOGI(TAG, "Zigbee stack started; waiting for network join");
    esp_zigbee_launch_mainloop();
    ESP_LOGE(TAG, "Zigbee mainloop stopped");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Zigbee-only test starting (Wi-Fi disabled)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_flash_init_partition("zb_storage"));

    if (xTaskCreate(zigbee_task, "zigbee_task", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }

    for (uint32_t n = 1;; ++n) {
        ESP_LOGI(TAG, "[%03lu] Zigbee: %s", (unsigned long)n,
                 joined ? "JOINED" : "NOT JOINED");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
