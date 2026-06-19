#include <stdio.h>

#include "nvs_flash.h"
#include "identity_manager.h"
#include "core1_engine.h"
#include "network_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_log.h>

static const char *TAG = "Main";

void app_main(void)
{
    /* ── Earliest possible UART log — confirms bootloader handed off OK ── */
    printf("\n\n=== BOOTING ESP32 SMART RELAY ===\n\n");
    ESP_LOGI("BOOT", "System Starting!");

    /* ── Boot Banner ───────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  Booting Smart Relay MVP...             ");
    ESP_LOGI(TAG, "  32-Channel Dual-Core Engine            ");
    ESP_LOGI(TAG, "=========================================");

    /* ── Step 1: Initialize NVS (required by identity_manager) ────────── */
    esp_err_t err = hal_nvs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: NVS init failed (%s). Halting.", esp_err_to_name(err));
        return;
    }

    /* ── Step 2: Launch Core 1 Hardware Engine ─────────────────────────── */
    /*            Internally calls: ipc_manager_init() → hal_spi_init()    */
    /*            → xTaskCreatePinnedToCore() for both hardware tasks.      */
    err = core1_engine_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: Core 1 engine init failed (%s). Halting.", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Core 1 hardware engine is live. Launching Core 0 network manager...");

    /* ── Step 3: Launch Core 0 Network Manager (Phase 3 Mocked Tuya MQTT) ─── */
    err = network_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: Network manager init failed (%s). Halting.", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "All subsystems nominal. Dual-core engine running.");

    /* ── Core 0 Idle Loop ───────────────────────────────────────────────── */
    /*   Core 0 tasks (uplink_disp, cloud_dummy) handle their own loops.      */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
