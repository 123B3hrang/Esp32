#include "identity_manager.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>
#include "sdkconfig.h"

#define TUYA_NVS_NAMESPACE "storage"

static const char *TAG = "IdentityManager";

esp_err_t hal_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase. Erasing...");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS flash initialized successfully.");
    } else {
        ESP_LOGE(TAG, "NVS flash initialization failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t load_device_identity(char* out_pid, size_t max_pid_len, 
                               char* out_uuid, size_t max_uuid_len, 
                               char* out_auth_key, size_t max_auth_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TUYA_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found.", TUYA_NVS_NAMESPACE);

#ifdef CONFIG_APP_USE_DUMMY_CREDENTIALS
        ESP_LOGW(TAG, "==================================================");
        ESP_LOGW(TAG, "WARNING: NVS not provisioned!");
        ESP_LOGW(TAG, "Using Open-Source Dev Dummy Credentials!");
        ESP_LOGW(TAG, "==================================================");

        // Safely copy Kconfig dummy values ensuring null-termination
        strncpy(out_pid, CONFIG_APP_DUMMY_PID, max_pid_len - 1);
        out_pid[max_pid_len - 1] = '\0';

        strncpy(out_uuid, CONFIG_APP_DUMMY_UUID, max_uuid_len - 1);
        out_uuid[max_uuid_len - 1] = '\0';

        strncpy(out_auth_key, CONFIG_APP_DUMMY_AUTH_KEY, max_auth_len - 1);
        out_auth_key[max_auth_len - 1] = '\0';

        return ESP_OK;
#else
        ESP_LOGE(TAG, "NVS not provisioned and dummy credentials disabled. FATAL!");
        return ESP_ERR_NVS_NOT_FOUND;
#endif
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;

    // --- Read PID ---
    err = nvs_get_str(handle, "pid", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_pid_len) {
        err = nvs_get_str(handle, "pid", out_pid, &required_size);
    } else {
        ESP_LOGE(TAG, "Failed to read PID or buffer too small. Req: %d, Max: %d", required_size, max_pid_len);
        nvs_close(handle);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    // --- Read UUID ---
    err = nvs_get_str(handle, "uuid", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_uuid_len) {
        err = nvs_get_str(handle, "uuid", out_uuid, &required_size);
    } else {
        ESP_LOGE(TAG, "Failed to read UUID or buffer too small. Req: %d, Max: %d", required_size, max_uuid_len);
        nvs_close(handle);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    // --- Read Auth Key ---
    err = nvs_get_str(handle, "auth_key", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_auth_len) {
        err = nvs_get_str(handle, "auth_key", out_auth_key, &required_size);
    } else {
        ESP_LOGE(TAG, "Failed to read Auth Key or buffer too small. Req: %d, Max: %d", required_size, max_auth_len);
        nvs_close(handle);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Factory credentials successfully loaded from NVS storage.");
    
    return ESP_OK;
}