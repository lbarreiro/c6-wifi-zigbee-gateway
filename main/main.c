#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_gateway";
#define STATUS_INTERVAL_MS 5000

static esp_err_t init_zigbee_storage(void)
{
    esp_err_t err = nvs_flash_init_partition("zb_storage");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase_partition("zb_storage");
        if (err == ESP_OK) err = nvs_flash_init_partition("zb_storage");
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Zigbee NVS init failed: %s (0x%x)", esp_err_to_name(err), err);
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

    ESP_LOGI(TAG, "Zigbee-only connectivity test starting");
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

    /* Autostart enables the normal BDB startup/network-steering procedure. */
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

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Zigbee-only test starting");
    ESP_ERROR_CHECK(nvs_flash_init());

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }

    /* Status is emitted from a separate task so the Zigbee mainloop remains untouched. */
    for (uint32_t check = 1;; ++check) {
        bool joined = ezb_bdb_dev_joined();
        ESP_LOGI(TAG, "[%03lu] Zigbee: %s", (unsigned long)check, joined ? "JOINED" : "NOT JOINED");
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}
