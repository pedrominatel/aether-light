#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ultraled.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL 960

typedef struct {
    int gpio_num;
    uint16_t led_count;
    ultraled_color_order_t color_order;
    uint8_t brightness_percent;
} ultraled_manager_channel_config_t;

typedef struct {
    bool enabled;
    ultraled_model_t led_model;
    uint8_t channel_count;
    ultraled_manager_channel_config_t channels[ULTRALED_MAX_CHANNELS];
} ultraled_manager_config_t;

typedef enum {
    ULTRALED_MANAGER_DISABLED = 0,
    ULTRALED_MANAGER_ACTIVE,
    ULTRALED_MANAGER_ERROR,
} ultraled_manager_state_t;

/* Loads the NVS configuration and starts UltraLED when it is enabled. */
esp_err_t ultraled_manager_init(void);

/* Validates application and hardware constraints without changing NVS. */
esp_err_t ultraled_manager_validate_config(const ultraled_manager_config_t *config);

/* Returns the fixed, board-specific GPIO allowlist used by validation and the UI. */
const int *ultraled_manager_get_allowed_gpios(size_t *count);
bool ultraled_manager_is_gpio_allowed(int gpio_num);

/* Persists a validated configuration. It is applied on the next boot. */
esp_err_t ultraled_manager_save_config(const ultraled_manager_config_t *config);

void ultraled_manager_get_config(ultraled_manager_config_t *config);
ultraled_manager_state_t ultraled_manager_get_state(void);
esp_err_t ultraled_manager_get_last_error(void);

/* The handle remains valid until restart; NULL means UltraLED is not active. */
ultraled_handle_t ultraled_manager_get_handle(void);

#ifdef __cplusplus
}
#endif
