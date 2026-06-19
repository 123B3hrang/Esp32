#ifndef IDENTITY_MANAGER_H
#define IDENTITY_MANAGER_H

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the default NVS partition.
 * Handles erase-and-retry logic if partition is corrupted or version changed.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hal_nvs_init(void);

/**
 * @brief Load Tuya identity credentials from NVS. 
 * Falls back to Kconfig dummy credentials if NVS is unprovisioned and open-source fallback is enabled.
 * * @param out_pid Buffer to store Product ID
 * @param max_pid_len Maximum size of out_pid buffer
 * @param out_uuid Buffer to store UUID
 * @param max_uuid_len Maximum size of out_uuid buffer
 * @param out_auth_key Buffer to store Auth Key
 * @param max_auth_len Maximum size of out_auth_key buffer
 * @return esp_err_t ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if missing and fallback disabled.
 */
esp_err_t load_device_identity(char* out_pid, size_t max_pid_len, 
                               char* out_uuid, size_t max_uuid_len, 
                               char* out_auth_key, size_t max_auth_len);

#ifdef __cplusplus
}
#endif

#endif // IDENTITY_MANAGER_H