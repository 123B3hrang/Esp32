#ifndef IPC_MANAGER_H
#define IPC_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* DOWNLINK: Core 0 (Network Engine) -> Core 1 (Hardware Engine)              */
/* ========================================================================== */
typedef enum {
    HW_CMD_UPDATE_RELAYS,     /*!< Tuya Cloud updates one or more relays */
    HW_CMD_REQ_BULK_SYNC,     /*!< Core 0 requests dirty mask on network reconnect */
    HW_CMD_CLEAR_DIRTY_MASK   /*!< Core 0 successfully reported bulk sync, clear mask */
} hardware_cmd_t;

typedef struct {
    hardware_cmd_t cmd;
    uint32_t relay_mask;      /*!< Bitmask of relays to update (1 = update, 0 = ignore) */
    uint32_t relay_state;     /*!< Target states (1 = ON, 0 = OFF) for the masked relays */
} hardware_event_t;

/* ========================================================================== */
/* UPLINK: Core 1 (Hardware Engine) -> Core 0 (Network Engine)                */
/* ========================================================================== */
typedef enum {
    UP_EVT_LOCAL_CHANGE,      /*!< Local physical button toggled a relay */
    UP_EVT_BULK_SYNC_DATA     /*!< Core 1 reporting dirty mask for offline sync */
} uplink_evt_t;

typedef struct {
    uplink_evt_t type;
    uint32_t current_state;   /*!< Definitive absolute 32-bit state of all relays */
    uint32_t dirty_mask;      /*!< Mask of relays changed while offline (for Bulk Sync) */
} uplink_event_t;

/* Global FreeRTOS Queue Handles */
extern QueueHandle_t hardware_queue;
extern QueueHandle_t uplink_queue;

/**
 * @brief Initializes the Inter-Process Communication queues safely in FreeRTOS Heap.
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM on heap failure.
 */
esp_err_t ipc_manager_init(void);

#ifdef __cplusplus
}
#endif

#endif // IPC_MANAGER_H