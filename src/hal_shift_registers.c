#include "hal_shift_registers.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <rom/ets_sys.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HAL_SPI";

// --- Hardware Pin Definitions ---
#define SPI_MOSI_PIN 13
#define SPI_MISO_PIN 12
#define SPI_SCLK_PIN 14

#define LATCH_PIN    15
#define OE_PIN       4
#define PL_PIN       5

// --- Global SPI Device Handles ---
static spi_device_handle_t spi_out_handle;
static spi_device_handle_t spi_in_handle;

esp_err_t hal_spi_init(void) {
    ESP_LOGI(TAG, "Initializing Shared SPI Bus and Shift Register GPIOs...");

    // 1. Configure Control GPIOs
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LATCH_PIN) | (1ULL << OE_PIN) | (1ULL << PL_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&io_conf);

    // 2. Boot Safety Constraints
    gpio_set_level(OE_PIN, 1);    // OE HIGH: Disable relay outputs immediately
    gpio_set_level(LATCH_PIN, 0); // LATCH LOW: Ready state
    gpio_set_level(PL_PIN, 1);    // PL HIGH: Normal shift mode

    // 3. Initialize Shared SPI Bus (SPI2_HOST)
    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32 // Enforce 32 bytes max memory size
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
        return err;
    }

    // 4. Configure Output Device (74HC595)
    spi_device_interface_config_t devcfg_out = {
        .clock_speed_hz = 5 * 1000 * 1000, // 5 MHz
        .mode = 0,                         // SPI Mode 0
        .spics_io_num = -1,                // Explicit manual LATCH
        .queue_size = 1
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg_out, &spi_out_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add 74HC595 to SPI Bus: %s", esp_err_to_name(err));
        return err;
    }

    // 5. Configure Input Device (74HC165)
    spi_device_interface_config_t devcfg_in = {
        .clock_speed_hz = 1 * 1000 * 1000, // 1 MHz
        .mode = 0,                         // SPI Mode 0
        .spics_io_num = -1,                // Explicit manual PL
        .queue_size = 1
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg_in, &spi_in_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add 74HC165 to SPI Bus: %s", esp_err_to_name(err));
        return err;
    }

    // 6. Clear logic safely before enabling Output Enable (OE)
    hal_74hc595_update(0x00000000);
    gpio_set_level(OE_PIN, 0); // OE LOW: Safely enable outputs
    
    ESP_LOGI(TAG, "HAL SPI Initialization Complete.");
    return ESP_OK;
}

esp_err_t hal_74hc595_update(uint32_t relay_states) {
    // Memory Safety: Explicit 4-byte buffer (prevents stack overflow)
    uint8_t tx_buffer[4];
    tx_buffer[0] = (uint8_t)((relay_states >> 24) & 0xFF);
    tx_buffer[1] = (uint8_t)((relay_states >> 16) & 0xFF);
    tx_buffer[2] = (uint8_t)((relay_states >> 8)  & 0xFF);
    tx_buffer[3] = (uint8_t)(relay_states & 0xFF);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 32; // 32 bits
    t.tx_buffer = tx_buffer;

    // Thread Safety: Lock the shared SPI bus
    esp_err_t err = spi_device_acquire_bus(spi_out_handle, portMAX_DELAY);
    if (err != ESP_OK) return err;

    // Transmit
    err = spi_device_polling_transmit(spi_out_handle, &t);
    
    // Thread Safety: Release the shared SPI bus
    spi_device_release_bus(spi_out_handle);

    if (err == ESP_OK) {
        // Pulse LATCH: LOW -> HIGH -> 1us delay -> LOW
        gpio_set_level(LATCH_PIN, 0);
        gpio_set_level(LATCH_PIN, 1);
        ets_delay_us(1);
        gpio_set_level(LATCH_PIN, 0);
    }

    return err;
}

esp_err_t hal_74hc165_read(uint32_t *button_states) {
    if (button_states == NULL) return ESP_ERR_INVALID_ARG;

    // Memory Safety: Explicit 4-byte buffer initialized to zero
    uint8_t rx_buffer[4] = {0};

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 32; // 32 bits
    t.tx_buffer = NULL;
    t.rx_buffer = rx_buffer;

    // Thread Safety: Lock the shared SPI bus
    esp_err_t err = spi_device_acquire_bus(spi_in_handle, portMAX_DELAY);
    if (err != ESP_OK) return err;

    // Pulse PL: LOW -> 1us delay -> HIGH (Latch physical pins into shift registers)
    gpio_set_level(PL_PIN, 0);
    ets_delay_us(1);
    gpio_set_level(PL_PIN, 1);

    // Receive
    err = spi_device_polling_transmit(spi_in_handle, &t);
    
    // Thread Safety: Release the shared SPI bus
    spi_device_release_bus(spi_in_handle);

    if (err == ESP_OK) {
        // Safely map cascaded bytes into absolute 32-bit state
        *button_states = ((uint32_t)rx_buffer[0] << 24) |
                         ((uint32_t)rx_buffer[1] << 16) |
                         ((uint32_t)rx_buffer[2] << 8)  |
                         ((uint32_t)rx_buffer[3]);
    }

    return err;
}