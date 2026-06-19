#ifndef CORE1_ENGINE_H
#define CORE1_ENGINE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Core 1 Hardware Engine.
 *
 * Performs the following in order:
 *   1. Calls ipc_manager_init()  — creates FreeRTOS dual-core queues.
 *   2. Calls hal_spi_init()      — initialises SPI bus and shift-register GPIOs.
 *   3. Creates task_spi_output   — high-priority SPI write task (pinned to Core 1).
 *   4. Creates task_input_poll   — medium-priority button poll task (pinned to Core 1).
 *
 * Must be called from app_main() after hal_nvs_init().
 *
 * @return esp_err_t  ESP_OK on success; propagates underlying driver errors.
 */
esp_err_t core1_engine_init(void);

#ifdef __cplusplus
}
#endif

#endif // CORE1_ENGINE_H
