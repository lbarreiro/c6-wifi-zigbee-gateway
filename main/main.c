#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_gateway";

#define RADIO_TEST_CYCLES 10
#define ZIGBEE_PHASE_MS 10000
#define WIFI_PHASE_MS 3000

static esp_netif_t *ap_netif = NULL;
static bool wifi_initialized = false;
static volatile bool zigbee_started = false;
static volatile bool zigbee_mainloop_exited = false;
static volatile bool zigbee_stop_requested = false;
static volatile esp_err_t zigbee_deinit_result = ESP_FAIL;

static esp_err_t init_wifi_once(void)
{
    if (wifi_initialized) return ESP_OK;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) return ESP_FAIL;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t ap_config = { .ap = { .ssid = "C6-Zigbee-Gateway", .ssid_len = sizeof("C6-Zigbee-Gateway") - 1, .channel = 1, .password = "Beatriz77", .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t wifi_on(void)
{
    ESP_ERROR_CHECK(init_wifi_once());
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) ESP_LOGI(TAG, "[WIFI] ON  | Zigbee OFF");
    else if (err == ESP_ERR_WIFI_STATE) err = ESP_OK;
    else ESP_LOGE(TAG, "[WIFI] start failed: %s (0x%x)", esp_err_to_name(err), err);
    return err;
}

static esp_err_t wifi_off(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGI(TAG, "[WIFI] OFF | Zigbee starting");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "[WIFI] stop failed: %s (0x%x)", esp_err_to_name(err), err);
    return err;
}

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

/* This callback executes inside the Zigbee main-loop context.  Deinitializing
   from here avoids trying to stop the mainloop from a competing task. */
static void zigbee_stop_callback(void *ctx)
{
    ESP_LOGI(TAG, "[ZIGBEE] stop callback executing in mainloop");
    zigbee_deinit_result = esp_zigbee_deinit();
    zigbee_started = false;
    zigbee_stop_requested = true;
    ESP_LOGI(TAG, "[ZIGBEE] deinit from mainloop returned: %s (0x%x)",
             esp_err_to_name(zigbee_deinit_result), zigbee_deinit_result);
}

static void zigbee_main_task(void *arg)
{
    esp_zigbee_config_t config = {
        .platform_config = { .storage_partition_name = "zb_storage", .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE } },
        .device_config = { .device_type = EZB_NWK_DEVICE_TYPE_ROUTER, .install_code_policy = false, .zczr_config = { .max_children = 10 } },
    };

    zigbee_mainloop_exited = false;
    zigbee_started = false;
    zigbee_stop_requested = false;
    zigbee_deinit_result = ESP_FAIL;

    ESP_LOGI(TAG, "[ZIGBEE] initializing");
    if (init_zigbee_storage() != ESP_OK) goto failed;
    if (esp_zigbee_init(&config) != ESP_OK) goto failed;
    if (esp_zigbee_start(false) != ESP_OK) { esp_zigbee_deinit(); goto failed; }

    zigbee_started = true;
    ESP_LOGI(TAG, "[ZIGBEE] ON  | Wi-Fi OFF");

    esp_err_t loop_err = esp_zigbee_launch_mainloop();
    ESP_LOGI(TAG, "[ZIGBEE] mainloop returned: %s (0x%x)", esp_err_to_name(loop_err), loop_err);

    zigbee_started = false;
    zigbee_mainloop_exited = true;
    vTaskDelete(NULL);
    return;

failed:
    zigbee_mainloop_exited = true;
    ESP_LOGE(TAG, "[ZIGBEE] failed to start");
    vTaskDelete(NULL);
}

static bool start_zigbee_and_wait(void)
{
    zigbee_mainloop_exited = false;
    if (xTaskCreate(zigbee_main_task, "zigbee_main", 6144, NULL, 5, NULL) != pdPASS) return false;
    for (int i = 0; i < 500; ++i) {
        if (zigbee_started) return true;
        if (zigbee_mainloop_exited) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "[ZIGBEE] timed out waiting for start");
    return false;
}

static bool stop_zigbee_and_wait(void)
{
    if (!zigbee_started) return true;

    ESP_LOGI(TAG, "[ZIGBEE] OFF requested | scheduling stop callback");

    /* Post the deinit into the Zigbee task itself.  The callback runs in the
       mainloop context, where Zigbee APIs are allowed without taking the lock. */
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(3000))) {
        ESP_LOGE(TAG, "[ZIGBEE] failed to acquire lock to schedule stop");
        return false;
    }
    esp_err_t post_err = esp_zigbee_task_queue_post(zigbee_stop_callback, NULL);
    esp_zigbee_lock_release();

    if (post_err != ESP_OK) {
        ESP_LOGE(TAG, "[ZIGBEE] failed to post stop callback: %s (0x%x)", esp_err_to_name(post_err), post_err);
        return false;
    }

    for (int i = 0; i < 500; ++i) {
        if (zigbee_mainloop_exited) {
            ESP_LOGI(TAG, "[ZIGBEE] OFF confirmed; deinit=%s (0x%x)",
                     esp_err_to_name(zigbee_deinit_result), zigbee_deinit_result);
            return zigbee_deinit_result == ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "[ZIGBEE] timed out waiting for mainloop exit");
    return false;
}

static void radio_test_task(void *arg)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Phase 5: Radio alternation stress test");
    ESP_LOGI(TAG, "Cycles: %d | Zigbee: %ds | Wi-Fi: %ds", RADIO_TEST_CYCLES, ZIGBEE_PHASE_MS / 1000, WIFI_PHASE_MS / 1000);
    ESP_LOGI(TAG, "========================================");

    for (int cycle = 1; cycle <= RADIO_TEST_CYCLES; ++cycle) {
        ESP_LOGI(TAG, "[%03d] Starting Zigbee phase", cycle);
        if (!start_zigbee_and_wait()) { ESP_LOGE(TAG, "[%03d] TEST FAILED: Zigbee did not start", cycle); goto fail; }
        ESP_LOGI(TAG, "[%03d] Zigbee stable for %d seconds", cycle, ZIGBEE_PHASE_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(ZIGBEE_PHASE_MS));

        if (!stop_zigbee_and_wait()) { ESP_LOGE(TAG, "[%03d] TEST FAILED: Zigbee did not stop", cycle); goto fail; }

        if (wifi_on() != ESP_OK) { ESP_LOGE(TAG, "[%03d] TEST FAILED: Wi-Fi did not start", cycle); goto fail; }
        ESP_LOGI(TAG, "[%03d] Wi-Fi stable for %d seconds", cycle, WIFI_PHASE_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(WIFI_PHASE_MS));

        if (wifi_off() != ESP_OK) { ESP_LOGE(TAG, "[%03d] TEST FAILED: Wi-Fi did not stop", cycle); goto fail; }
        ESP_LOGI(TAG, "[%03d] COMPLETE", cycle);
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "TEST COMPLETE: %d radio cycles without failure", RADIO_TEST_CYCLES);
    ESP_LOGI(TAG, "========================================");
    vTaskDelete(NULL);
    return;

fail:
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "TEST FAILED - stopping radio scheduler");
    ESP_LOGE(TAG, "========================================");
    esp_wifi_stop();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Wi-Fi + Zigbee gateway starting");
    ESP_LOGI(TAG, "Phase 5: Wi-Fi/Zigbee radio alternation test");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (init_wifi_once() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
        return;
    }

    xTaskCreate(radio_test_task, "radio_test", 6144, NULL, 5, NULL);
}
