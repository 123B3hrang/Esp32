/**
 * @file network_manager.c
 * @brief Phase 3 Network Manager — Core 0 Cloud Interface (Mocked Tuya MQTT).
 *
 * Task topology (both tasks pinned to Core 0):
 *
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │  core0_uplink_dispatcher_task  (priority 4)                          │
 *   │    Blocks on uplink_queue.                                           │
 *   │    UP_EVT_LOCAL_CHANGE  → builds {"<dp_id>": <bool>} JSON per bit   │
 *   │    in dirty_mask, calls tuyalink_thing_property_report() (mocked).  │
 *   │    UP_EVT_BULK_SYNC_DATA → reports all 8 channels in one payload.   │
 *   ├──────────────────────────────────────────────────────────────────────┤
 *   │  core0_dummy_cloud_task  (priority 2)                                │
 *   │    Simulates Tuya MQTT downlinks every 15 s:                         │
 *   │      t=0s:  {"1":true,"4":true}  → turns ON relays 1 & 4           │
 *   │      t=15s: {"1":false,"4":false} → turns OFF relays 1 & 4          │
 *   │    Parses JSON, builds hardware_event_t, pushes to hardware_queue.  │
 *   └──────────────────────────────────────────────────────────────────────┘
 */

#include "network_manager.h"
#include "ipc_manager.h"
#include "identity_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <esp_log.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

/* ─── Configuration ─────────────────────────────────────────────────────── */

#define TAG             "NetMgr"
#define TASK_STACK      4096
#define DOWNLINK_PERIOD_MS 15000

/** Module-level device ID — populated during network_manager_init(). */
static char s_dev_id[64] = "wokwi_sim_device_001";

/* ─── Mock Tuya SDK Stub ─────────────────────────────────────────────────── */

/** Minimal mock replacing tuya_iot.h — remove when real SDK is integrated. */
typedef struct { int dummy; } tuya_mqtt_context_t;

static tuya_mqtt_context_t s_tuya_ctx;

/**
 * @brief Mock Tuya property-report function.
 *
 * In production this would call the real Tuya IoTOS SDK to publish MQTT
 * upstream.  Here it logs the payload so the Wokwi terminal shows the JSON
 * that would be sent to Tuya Cloud.
 */
static void tuyalink_thing_property_report(void *ctx, const char *dev_id,
                                           const char *json)
{
    (void)ctx;
    ESP_LOGW("TUYA_MOCK", "[UPLINK → Cloud] dev=%s  payload=%s", dev_id, json);
}

/* ─── Task: core0_uplink_dispatcher_task ────────────────────────────────── */

/**
 * @brief Core 0 uplink dispatcher — drains uplink_queue and sends to cloud.
 *
 * For each UP_EVT_LOCAL_CHANGE event it iterates the dirty_mask and builds
 * a compact JSON object {"<dp_id>": <bool>} for every toggled channel
 * (dp_id = bit_position + 1, 1-indexed to match Tuya data-point convention).
 */
static void core0_uplink_dispatcher_task(void *pvParameters)
{
    (void)pvParameters;
    uplink_event_t event;

    ESP_LOGI(TAG, "core0_uplink_dispatcher_task started (Core %d).",
             xPortGetCoreID());

    for (;;) {
        if (xQueueReceive(uplink_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Build a JSON object for every bit set in dirty_mask. */
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            ESP_LOGE(TAG, "cJSON_CreateObject OOM — dropping uplink event.");
            continue;
        }

        uint32_t mask = (event.type == UP_EVT_BULK_SYNC_DATA)
                        ? 0xFF          /* report all 8 channels */
                        : event.dirty_mask;

        for (int bit = 0; bit < 8; bit++) {
            if (!(mask & (1UL << bit))) continue;

            char dp_key[8];
            snprintf(dp_key, sizeof(dp_key), "%d", bit + 1); /* 1-indexed dp */
            bool state = (event.current_state >> bit) & 1U;
            cJSON_AddBoolToObject(root, dp_key, state);
        }

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            tuyalink_thing_property_report(&s_tuya_ctx, s_dev_id, json_str);
            free(json_str);
        } else {
            ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (OOM).");
        }
        cJSON_Delete(root);
    }
}

/* ─── Task: core0_dummy_cloud_task ──────────────────────────────────────── */

/**
 * @brief Helper — parse a JSON downlink string and push relay commands.
 *
 * Keys are string dp_ids (1-indexed); values are JSON booleans.
 * Each key-value pair becomes one hardware_event_t with a single-bit mask.
 */
static void process_downlink(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_Parse failed for: %s", json_str);
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        int dp_id = atoi(item->string); /* dp_id is the JSON key */
        if (dp_id < 1 || dp_id > 8) {
            ESP_LOGW(TAG, "dp_id %d out of range [1..8], skipping.", dp_id);
            continue;
        }

        bool state = cJSON_IsTrue(item);
        uint32_t bit_pos = (uint32_t)(dp_id - 1);

        hardware_event_t hw_evt = {
            .cmd         = HW_CMD_UPDATE_RELAYS,
            .relay_mask  = 1UL << bit_pos,
            .relay_state = state ? (1UL << bit_pos) : 0UL,
        };

        ESP_LOGI(TAG, "[DOWNLINK ← Cloud] dp=%d → relay %d = %s",
                 dp_id, dp_id, state ? "ON" : "OFF");

        if (xQueueSend(hardware_queue, &hw_evt, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "hardware_queue full — cloud command dp=%d dropped!", dp_id);
        }
    }

    cJSON_Delete(root);
}

/**
 * @brief Core 0 dummy cloud simulator — fires mock MQTT downlinks every 15 s.
 *
 * Stress-tests cloud-to-local synchronisation by alternately turning relays
 * 1 and 4 ON/OFF through the hardware_queue → task_spi_output pipeline.
 * This validates that Core 0 and Core 1 can simultaneously handle:
 *   - Core 0: cloud downlink arriving via hardware_queue
 *   - Core 1: physical button press arriving from 74HC165
 * without race conditions on current_relay_states.
 */
static void core0_dummy_cloud_task(void *pvParameters)
{
    (void)pvParameters;

    /* Allow Core 1 tasks to fully start before sending the first downlink. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "core0_dummy_cloud_task started (Core %d). "
                  "First downlink in 3 s...", xPortGetCoreID());

    for (;;) {
        /* ── Turn ON relays 1 and 4 ──────────────────────────────────────── */
        const char *on_payload  = "{\"1\":true,\"4\":true}";
        ESP_LOGI(TAG, "[DOWNLINK ← Cloud] raw payload: %s", on_payload);
        process_downlink(on_payload);

        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_PERIOD_MS));

        /* ── Turn OFF relays 1 and 4 ─────────────────────────────────────── */
        const char *off_payload = "{\"1\":false,\"4\":false}";
        ESP_LOGI(TAG, "[DOWNLINK ← Cloud] raw payload: %s", off_payload);
        process_downlink(off_payload);

        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_PERIOD_MS));
    }
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

esp_err_t network_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Phase 3 Network Manager (Mocked Tuya MQTT)...");

    /* Load device credentials — logs them; fails gracefully in simulation. */
    char pid[32]     = {0};
    char uuid[48]    = {0};
    char auth_key[64]= {0};
    esp_err_t err = load_device_identity(pid, sizeof(pid),
                                         uuid, sizeof(uuid),
                                         auth_key, sizeof(auth_key));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device identity loaded: PID=%s  UUID=%s", pid, uuid);
        /* Copy UUID as device ID for uplink reports. */
        strncpy(s_dev_id, uuid, sizeof(s_dev_id) - 1);
    } else {
        ESP_LOGW(TAG, "Identity load failed (%s) — proceeding with mock credentials.",
                 esp_err_to_name(err));
        /* s_dev_id already holds the Wokwi fallback value set at declaration. */
    }

    /* ── Task 1: Uplink dispatcher (drains uplink_queue → cloud) ──────── */
    BaseType_t ret = xTaskCreatePinnedToCore(
        core0_uplink_dispatcher_task,
        "uplink_disp",
        TASK_STACK,
        NULL,
        4,      /* priority: below SPI output (5), above dummy cloud (2) */
        NULL,
        0       /* Core 0 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create core0_uplink_dispatcher_task (OOM).");
        return ESP_ERR_NO_MEM;
    }

    /* ── Task 2: Dummy cloud simulator (periodic downlinks via hardware_queue) */
    ret = xTaskCreatePinnedToCore(
        core0_dummy_cloud_task,
        "cloud_dummy",
        TASK_STACK,
        NULL,
        2,      /* lowest priority: best-effort simulation */
        NULL,
        0       /* Core 0 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create core0_dummy_cloud_task (OOM).");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Network Manager online. Both Core 0 cloud tasks running.");
    return ESP_OK;
}
