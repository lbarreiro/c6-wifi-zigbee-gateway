#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
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

static void bdb_start_cb(uint8_t mode_mask)
{
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode_mask);
    if (err != ESP_OK) ESP_LOGE(TAG, "BDB commissioning failed to start: %s (0x%x)", esp_err_to_name(err), err);
}

/* Standard ESP Zigbee BDB signal flow: initialize first, then network steering.
 * This is the flow used by Espressif's router examples and is important for
 * interoperability with Zigbee 3.0 coordinators such as Zigbee2MQTT. */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    if (!signal_struct || !signal_struct->p_app_signal) return;

    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee stack initialized; starting BDB initialization");
            bdb_start_cb(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            ESP_LOGI(TAG, "Zigbee startup: %s | status=%s | factory_new=%s",
                     esp_zb_zdo_signal_to_string(sig_type), esp_err_to_name(status),
                     esp_zb_bdb_is_factory_new() ? "yes" : "no");
            if (status == ESP_OK && esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting Zigbee 3.0 network steering");
                bdb_start_cb(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (status == ESP_OK) {
                esp_zb_ieee_addr_t ext_pan_id;
                esp_zb_get_extended_pan_id(ext_pan_id);
                ESP_LOGI(TAG, "JOINED Zigbee network: PAN=0x%04hx Channel=%d ShortAddr=0x%04hx",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
                ESP_LOGI(TAG, "Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         ext_pan_id[7], ext_pan_id[6], ext_pan_id[5], ext_pan_id[4],
                         ext_pan_id[3], ext_pan_id[2], ext_pan_id[1], ext_pan_id[0]);
            } else {
                ESP_LOGW(TAG, "Network steering failed: %s (0x%x); retrying in 2s",
                         esp_err_to_name(status), status);
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
            ESP_LOGI(TAG, "Zigbee device announcement received");
            break;

        default:
            ESP_LOGI(TAG, "Zigbee signal: %s (0x%x), status=%s",
                     esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(status));
            break;
    }
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

    ESP_LOGI(TAG, "ESP32-C6 Zigbee-only connectivity test starting");
    ESP_LOGI(TAG, "Role: Router | target: Zigbee 3.0 coordinator / Zigbee2MQTT");

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

    /* Do not use esp_zigbee_start(true): BDB initialization and steering must
     * be driven by the Zigbee application signal handler so failures are visible
     * and steering can be retried. */
    err = esp_zigbee_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s (0x%x)", esp_err_to_name(err), err);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Zigbee stack started; waiting for Zigbee2MQTT permit-join");
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

    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return;
    }

    for (uint32_t check = 1;; ++check) {
        ESP_LOGI(TAG, "[%03lu] Zigbee stack alive; join state is reported by BDB signals", (unsigned long)check);
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}
