#ifndef HAL_SHIFT_REGISTERS_H
#define HAL_SHIFT_REGISTERS_H

#include "esp_err.h"
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the shared SPI2_HOST bus and configure GPIOs for shift
 * registers. Enforces boot safety constraints to prevent relay misfire.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hal_spi_init(void);

/**
 * @brief Thread-safe update of the 32-channel cascaded 74HC595 outputs.
 *
 * @param relay_states 32-bit absolute state of all relays.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hal_74hc595_update(uint32_t relay_states);

/**
 * @brief Thread-safe read of the 32-channel cascaded 74HC165 inputs.
 *
 * @param button_states Pointer to store the 32-bit absolute state of all
 * inputs.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hal_74hc165_read(uint32_t *button_states);

#ifdef __cplusplus
}
#endif

#endif // HAL_SHIFT_REGISTERS_H