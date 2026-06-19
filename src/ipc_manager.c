#include "ipc_manager.h"
#include <esp_log.h>

static const char *TAG = "IPC_Manager";

QueueHandle_t hardware_queue = NULL;
QueueHandle_t uplink_queue = NULL;

// Queue sizes
#define HARDWARE_QUEUE_LEN 10
#define UPLINK_QUEUE_LEN   32 // Larger to prevent dropping rapid physical button presses

esp_err_t ipc_manager_init(void) {
    ESP_LOGI(TAG, "Initializing Dual-Core IPC Bridge...");

    hardware_queue = xQueueCreate(HARDWARE_QUEUE_LEN, sizeof(hardware_event_t));
    if (hardware_queue == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to create Hardware Downlink Queue (Out of Heap)");
        return ESP_ERR_NO_MEM;
    }

    uplink_queue = xQueueCreate(UPLINK_QUEUE_LEN, sizeof(uplink_event_t));
    if (uplink_queue == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to create Tuya Uplink Queue (Out of Heap)");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IPC Dual-Core Queues successfully initialized.");
    return ESP_OK;
}