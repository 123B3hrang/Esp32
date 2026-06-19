/**
 * @file core1_engine.c
 * @brief Core 1 Hardware Engine — SPI output driver and physical input poller.
 *
 * Task topology (both tasks pinned to Core 1):
 *
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │  task_spi_output  (HIGH priority)                                    │
 *   │    Blocks forever on hardware_queue.                                 │
 *   │    On HW_CMD_UPDATE_RELAYS: applies mask+state to current_relay_     │
 *   │    states, then calls hal_74hc595_update() to push bits to 74HC595.  │
 *   ├──────────────────────────────────────────────────────────────────────┤
 *   │  task_input_poll  (MEDIUM priority)                                  │
 *   │    Polls 74HC165 every 10 ms.                                        │
 *   │    4-cycle bitwise debounce; only rising-edge (button-press)         │
 *   │    transitions produce events.                                       │
 *   │    On confirmed press: toggles current_relay_states bit, sends       │
 *   │    HW_CMD_UPDATE_RELAYS onto hardware_queue AND UP_EVT_LOCAL_CHANGE  │
 *   │    onto uplink_queue.                                                │
 *   └──────────────────────────────────────────────────────────────────────┘
 */

#include "core1_engine.h"
#include "hal_shift_registers.h"
#include "ipc_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <esp_log.h>
#include <stdint.h>

/* ─── Configuration ─────────────────────────────────────────────────────── */

#define TAG                     "Core1Engine"

/** Stack depth (words) for both tasks. */
#define TASK_STACK_DEPTH        4096

/** Priority levels — stays below the FreeRTOS timer daemon (priority 1). */
#define TASK_PRIO_SPI_OUTPUT    5   /*!< High: must drain the hw queue fast   */
#define TASK_PRIO_INPUT_POLL    3   /*!< Medium: 10 ms polling cadence        */

/** Debounce: number of consecutive identical samples required for a stable
 *  reading.  At 10 ms per cycle, 4 cycles ≈ 40 ms debounce window. */
#define DEBOUNCE_CYCLES         4

/** Input poll interval in milliseconds. */
#define INPUT_POLL_MS           10

/* ─── Shared state (Core 1 private) ─────────────────────────────────────── */

/**
 * Authoritative 32-bit relay shadow register.
 * Written by both tasks — safe because:
 *   • task_spi_output only writes it from a received hardware_event_t payload.
 *   • task_input_poll only writes it when a debounced press is confirmed and
 *     immediately mirrors the new value into hardware_queue so task_spi_output
 *     will update the physical outputs.
 * Both tasks are pinned to Core 1 and FreeRTOS cooperative scheduling ensures
 * task_spi_output (higher priority) pre-empts task_input_poll cleanly.
 */
static volatile uint32_t current_relay_states = 0;

/* ─── Task: task_spi_output ─────────────────────────────────────────────── */

/**
 * @brief High-priority SPI output driver task.
 *
 * Sits blocking on hardware_queue indefinitely.  For every
 * HW_CMD_UPDATE_RELAYS command received it:
 *   1. Applies the relay_mask / relay_state from the event to the shadow
 *      register (bit-manipulation, no full overwrite so partial updates work).
 *   2. Calls hal_74hc595_update() to clock the new state into the 74HC595
 *      chain.
 *
 * Other command types are acknowledged with a log entry and discarded
 * (future: bulk-sync handling will be added here).
 */
static void task_spi_output(void *pvParameters)
{
    (void)pvParameters;
    hardware_event_t event;

    ESP_LOGI(TAG, "task_spi_output started (Core %d).", xPortGetCoreID());

    for (;;) {
        /* Block indefinitely — no busy-waiting. */
        if (xQueueReceive(hardware_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.cmd) {
                case HW_CMD_UPDATE_RELAYS: {
                    /* Apply masked update to the shadow register. */
                    uint32_t new_state = (current_relay_states & ~event.relay_mask)
                                       | (event.relay_state   &  event.relay_mask);
                    current_relay_states = new_state;

                    esp_err_t err = hal_74hc595_update(current_relay_states);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "hal_74hc595_update failed: %s",
                                 esp_err_to_name(err));
                    }
                    break;
                }

                case HW_CMD_REQ_BULK_SYNC: {
                    /* Core 0 requesting full state for cloud re-sync. */
                    ESP_LOGI(TAG, "Bulk sync requested — current state: 0x%08lX",
                             (unsigned long)current_relay_states);
                    /* Send the current dirty state uplink. */
                    uplink_event_t up = {
                        .type          = UP_EVT_BULK_SYNC_DATA,
                        .current_state = current_relay_states,
                        .dirty_mask    = 0xFFFFFFFFUL, /* report all channels */
                    };
                    xQueueSend(uplink_queue, &up, 0);
                    break;
                }

                case HW_CMD_CLEAR_DIRTY_MASK:
                    /* Core 0 confirmed the sync was uploaded. */
                    ESP_LOGI(TAG, "Dirty mask cleared by Core 0.");
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown hardware command: %d", event.cmd);
                    break;
            }
        }
    }
}

/* ─── Task: task_input_poll ─────────────────────────────────────────────── */

/**
 * @brief Medium-priority physical button polling task with 4-cycle debounce.
 *
 * Algorithm:
 *   raw[]       — circular buffer of the last DEBOUNCE_CYCLES raw readings.
 *   stable      — the last confirmed stable (debounced) state.
 *
 *   Each 10 ms tick:
 *     1. Read raw 32-bit button word from 74HC165 chain.
 *     2. Store in ring buffer slot [tick % DEBOUNCE_CYCLES].
 *     3. Compute consensus = AND of all DEBOUNCE_CYCLES samples.
 *        A bit is considered pressed only if it is set in ALL recent samples.
 *     4. Detect rising edge: new_pressed = consensus & ~stable.
 *     5. For each set bit in new_pressed:
 *          a. Toggle the corresponding bit in current_relay_states.
 *          b. Post HW_CMD_UPDATE_RELAYS (full-mask, full-state) to
 *             hardware_queue so task_spi_output pushes it to hardware.
 *          c. Post UP_EVT_LOCAL_CHANGE to uplink_queue so Core 0 can
 *             report the change to Tuya Cloud.
 *     6. Update stable = consensus.
 */
static void task_input_poll(void *pvParameters)
{
    (void)pvParameters;

    uint32_t raw[DEBOUNCE_CYCLES] = {0};
    uint32_t stable               = 0;
    uint8_t  tick                 = 0;

    ESP_LOGI(TAG, "task_input_poll started (Core %d).", xPortGetCoreID());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));

        /* 1. Read raw button state from 74HC165 chain. */
        uint32_t raw_sample = 0;
        esp_err_t err = hal_74hc165_read(&raw_sample);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hal_74hc165_read failed: %s", esp_err_to_name(err));
            continue;
        }

        /* 2. Store sample in ring buffer. */
        raw[tick % DEBOUNCE_CYCLES] = raw_sample;
        tick++;

        /* 3. Compute consensus (all DEBOUNCE_CYCLES samples must agree). */
        uint32_t consensus = 0xFFFFFFFFUL;
        for (int i = 0; i < DEBOUNCE_CYCLES; i++) {
            consensus &= raw[i];
        }

        /* 4. Rising-edge detection: bits newly pressed (not seen in stable). */
        uint32_t new_pressed = consensus & ~stable;

        if (new_pressed != 0) {
            /* 5. Process each newly-pressed button independently. */
            for (uint8_t bit = 0; bit < 32; bit++) {
                if (!(new_pressed & (1UL << bit))) {
                    continue;
                }

                /* a. Toggle relay in shadow register. */
                current_relay_states ^= (1UL << bit);
                uint32_t snapshot = current_relay_states;

                ESP_LOGI(TAG, "Button %d pressed → relay %d toggled. "
                              "All states: 0x%08lX",
                         bit, bit, (unsigned long)snapshot);

                /* b. Send to hardware_queue → task_spi_output drives 74HC595. */
                hardware_event_t hw_evt = {
                    .cmd         = HW_CMD_UPDATE_RELAYS,
                    .relay_mask  = 0xFFFFFFFFUL, /* full absolute update */
                    .relay_state = snapshot,
                };
                if (xQueueSend(hardware_queue, &hw_evt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "hardware_queue full — SPI update dropped!");
                }

                /* c. Send to uplink_queue → Core 0 reports to Tuya Cloud. */
                uplink_event_t up_evt = {
                    .type          = UP_EVT_LOCAL_CHANGE,
                    .current_state = snapshot,
                    .dirty_mask    = (1UL << bit),
                };
                if (xQueueSend(uplink_queue, &up_evt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "uplink_queue full — cloud report dropped!");
                }
            }
        }

        /* 6. Advance stable baseline. */
        stable = consensus;
    }
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

esp_err_t core1_engine_init(void)
{
    ESP_LOGI(TAG, "Initializing Core 1 Hardware Engine...");

    /* Step 1: Create IPC queues. */
    esp_err_t err = ipc_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ipc_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Step 2: Initialize SPI bus and shift-register GPIOs. */
    err = hal_spi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_spi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Step 3: Create task_spi_output — pinned to Core 1. */
    BaseType_t ret = xTaskCreatePinnedToCore(
        task_spi_output,
        "spi_output",
        TASK_STACK_DEPTH,
        NULL,
        TASK_PRIO_SPI_OUTPUT,
        NULL,
        1   /* Core 1 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task_spi_output (OOM).");
        return ESP_ERR_NO_MEM;
    }

    /* Step 4: Create task_input_poll — pinned to Core 1. */
    ret = xTaskCreatePinnedToCore(
        task_input_poll,
        "input_poll",
        TASK_STACK_DEPTH,
        NULL,
        TASK_PRIO_INPUT_POLL,
        NULL,
        1   /* Core 1 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task_input_poll (OOM).");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Core 1 Hardware Engine online. Tasks running on Core 1.");
    return ESP_OK;
}
