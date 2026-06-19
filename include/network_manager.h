/**
 * @file network_manager.h
 * @brief Phase 3 Network Manager — Core 0 Cloud Interface (Mocked Tuya MQTT).
 *
 * Public API for the dual-task Core 0 cloud bridge:
 *   • core0_uplink_dispatcher_task — drains uplink_queue and serialises events
 *     to JSON for Tuya Cloud (mock in simulation; real SDK replaces the stub).
 *   • core0_dummy_cloud_task       — simulates periodic MQTT downlinks to stress-
 *     test cloud-to-local relay control through hardware_queue.
 */
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Phase 3 Network Manager.
 *
 * Loads device identity, then spawns both Core 0 cloud tasks:
 *   1. core0_uplink_dispatcher_task (priority 4, Core 0)
 *   2. core0_dummy_cloud_task       (priority 2, Core 0)
 *
 * Must be called after core1_engine_init() (IPC queues must exist first).
 *
 * @return esp_err_t  ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t network_manager_init(void);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_MANAGER_H
