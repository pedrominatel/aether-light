#include "ultraled_manager.h"

#include <stddef.h>
#include <string.h>

#include "board_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define ULTRALED_NVS_NAMESPACE "ultraled"
#define ULTRALED_NVS_KEY        "config"
#define ULTRALED_NVS_MAGIC      0x554c4346U
#define ULTRALED_NVS_VERSION_V1 1U
#define ULTRALED_NVS_VERSION    2U
#define ULTRALED_NVS_V1_CHANNELS 8U

typedef struct {
    uint8_t gpio_num;
    uint8_t color_order;
    uint8_t brightness_percent;
    uint8_t reserved;
    uint16_t led_count;
    uint16_t reserved2;
} ultraled_nvs_channel_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t enabled;
    uint8_t led_model;
    uint8_t channel_count;
    uint8_t reserved;
    ultraled_nvs_channel_v1_t channels[ULTRALED_NVS_V1_CHANNELS];
} ultraled_nvs_config_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t enabled;
    uint8_t led_model;
    uint8_t channel_count;
    uint8_t reserved;
    ultraled_nvs_channel_v1_t channels[ULTRALED_MAX_CHANNELS];
} ultraled_nvs_config_v2_t;

_Static_assert(sizeof(ultraled_nvs_channel_v1_t) == 8, "Unexpected UltraLED NVS channel layout");
_Static_assert(sizeof(ultraled_nvs_config_v1_t) == 76, "Unexpected UltraLED NVS config layout");
_Static_assert(sizeof(ultraled_nvs_config_v2_t) == 140, "Unexpected UltraLED NVS v2 config layout");

static const char *TAG = "ultraled_manager";

static SemaphoreHandle_t s_lock;
static ultraled_handle_t s_handle;
static ultraled_manager_config_t s_config;
static ultraled_manager_state_t s_state = ULTRALED_MANAGER_DISABLED;
static esp_err_t s_last_error = ESP_OK;

static void set_default_config(ultraled_manager_config_t *config)
{
    size_t gpio_count = 0;
    const int *allowed_gpios = board_hal_get_led_gpios(&gpio_count);
    memset(config, 0, sizeof(*config));
    config->enabled = false;
    config->led_model = ULTRALED_MODEL_WS2812;
    config->channel_count = 1;

    for (size_t channel = 0; channel < ULTRALED_MAX_CHANNELS; ++channel) {
        config->channels[channel].gpio_num = channel < gpio_count ? allowed_gpios[channel] : -1;
        config->channels[channel].led_count = 60;
        config->channels[channel].color_order = ULTRALED_COLOR_ORDER_MODEL_DEFAULT;
        config->channels[channel].brightness_percent = ULTRALED_MANAGER_DEFAULT_BRIGHTNESS_PERCENT;
    }
}

const int *ultraled_manager_get_allowed_gpios(size_t *count)
{
    return board_hal_get_led_gpios(count);
}

bool ultraled_manager_is_gpio_allowed(int gpio_num)
{
    size_t count = 0;
    const int *allowed_gpios = board_hal_get_led_gpios(&count);
    for (size_t i = 0; i < count; ++i) {
        if (gpio_num == allowed_gpios[i]) {
            return true;
        }
    }
    return false;
}

esp_err_t ultraled_manager_validate_config(const ultraled_manager_config_t *config)
{
    if (config == NULL || config->led_model < 0 || config->led_model >= ULTRALED_MODEL_MAX ||
        config->channel_count == 0 || config->channel_count > ULTRALED_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t channel = 0; channel < config->channel_count; ++channel) {
        const ultraled_manager_channel_config_t *channel_config = &config->channels[channel];
        if (!ultraled_manager_is_gpio_allowed(channel_config->gpio_num) ||
            channel_config->led_count == 0 ||
            channel_config->led_count > ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL ||
            channel_config->color_order < ULTRALED_COLOR_ORDER_MODEL_DEFAULT ||
            channel_config->color_order >= ULTRALED_COLOR_ORDER_MAX ||
            channel_config->brightness_percent > 100) {
            return ESP_ERR_INVALID_ARG;
        }

        for (size_t previous = 0; previous < channel; ++previous) {
            if (channel_config->gpio_num == config->channels[previous].gpio_num) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

static void encode_nvs_config(const ultraled_manager_config_t *config, ultraled_nvs_config_v2_t *stored)
{
    memset(stored, 0, sizeof(*stored));
    stored->magic = ULTRALED_NVS_MAGIC;
    stored->version = ULTRALED_NVS_VERSION;
    stored->size = sizeof(*stored);
    stored->enabled = config->enabled ? 1 : 0;
    stored->led_model = (uint8_t)config->led_model;
    stored->channel_count = config->channel_count;

    for (size_t channel = 0; channel < ULTRALED_MAX_CHANNELS; ++channel) {
        stored->channels[channel].gpio_num = (uint8_t)config->channels[channel].gpio_num;
        stored->channels[channel].color_order = (uint8_t)config->channels[channel].color_order;
        stored->channels[channel].brightness_percent = config->channels[channel].brightness_percent;
        stored->channels[channel].led_count = config->channels[channel].led_count;
    }
}

static esp_err_t decode_nvs_config_v2(const ultraled_nvs_config_v2_t *stored, ultraled_manager_config_t *config)
{
    if (stored->magic != ULTRALED_NVS_MAGIC || stored->version != ULTRALED_NVS_VERSION ||
        stored->size != sizeof(*stored) || stored->enabled > 1) {
        return ESP_ERR_INVALID_VERSION;
    }

    memset(config, 0, sizeof(*config));
    config->enabled = stored->enabled != 0;
    config->led_model = (ultraled_model_t)stored->led_model;
    config->channel_count = stored->channel_count;

    for (size_t channel = 0; channel < ULTRALED_MAX_CHANNELS; ++channel) {
        config->channels[channel].gpio_num = stored->channels[channel].gpio_num;
        config->channels[channel].led_count = stored->channels[channel].led_count;
        config->channels[channel].color_order = (ultraled_color_order_t)stored->channels[channel].color_order;
        config->channels[channel].brightness_percent = stored->channels[channel].brightness_percent;
    }

    return ultraled_manager_validate_config(config);
}

static esp_err_t decode_nvs_config_v1(const ultraled_nvs_config_v1_t *stored, ultraled_manager_config_t *config)
{
    if (stored->magic != ULTRALED_NVS_MAGIC || stored->version != ULTRALED_NVS_VERSION_V1 ||
        stored->size != sizeof(*stored) || stored->enabled > 1 ||
        stored->channel_count == 0 || stored->channel_count > ULTRALED_NVS_V1_CHANNELS) {
        return ESP_ERR_INVALID_VERSION;
    }

    set_default_config(config);
    config->enabled = stored->enabled != 0;
    config->led_model = (ultraled_model_t)stored->led_model;
    config->channel_count = stored->channel_count;
    for (size_t channel = 0; channel < ULTRALED_NVS_V1_CHANNELS; ++channel) {
        config->channels[channel].gpio_num = stored->channels[channel].gpio_num;
        config->channels[channel].led_count = stored->channels[channel].led_count;
        config->channels[channel].color_order = (ultraled_color_order_t)stored->channels[channel].color_order;
        config->channels[channel].brightness_percent = stored->channels[channel].brightness_percent;
    }
    return ultraled_manager_validate_config(config);
}

static esp_err_t write_nvs_config(const ultraled_manager_config_t *config)
{
    ultraled_nvs_config_v2_t stored;
    encode_nvs_config(config, &stored);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ULTRALED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, ULTRALED_NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t read_nvs_config(ultraled_manager_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ULTRALED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t stored_size = 0;
    err = nvs_get_blob(handle, ULTRALED_NVS_KEY, NULL, &stored_size);
    union {
        ultraled_nvs_config_v1_t v1;
        ultraled_nvs_config_v2_t v2;
    } stored = {0};
    if (err == ESP_OK) {
        if (stored_size > sizeof(stored)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = nvs_get_blob(handle, ULTRALED_NVS_KEY, &stored, &stored_size);
        }
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (stored_size == sizeof(stored.v2)) {
        return decode_nvs_config_v2(&stored.v2, config);
    }
    if (stored_size == sizeof(stored.v1)) {
        return decode_nvs_config_v1(&stored.v1, config);
    }
    return ESP_ERR_INVALID_SIZE;
}

esp_err_t ultraled_manager_save_config(const ultraled_manager_config_t *config)
{
    esp_err_t err = ultraled_manager_validate_config(config);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = write_nvs_config(config);
    if (err == ESP_OK) {
        s_config = *config;
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t start_ultraled(const ultraled_manager_config_t *config)
{
    ultraled_config_t driver_config = {
        .led_model = config->led_model,
        .channel_count = config->channel_count,
    };

    for (size_t channel = 0; channel < config->channel_count; ++channel) {
        driver_config.channels[channel] = (ultraled_channel_config_t) {
            .gpio_num = config->channels[channel].gpio_num,
            .led_count = config->channels[channel].led_count,
            .color_order = config->channels[channel].color_order,
        };
    }

    esp_err_t err = ultraled_new(&driver_config, &s_handle);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t channel = 0; channel < config->channel_count; ++channel) {
        uint8_t brightness = (uint8_t)(((uint16_t)config->channels[channel].brightness_percent * 255U + 50U) / 100U);
        err = ultraled_set_brightness(s_handle, channel, brightness);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    err = ultraled_clear_all(s_handle);
    for (size_t channel = 0; err == ESP_OK && channel < config->channel_count; ++channel) {
        err = ultraled_set_pixel(s_handle, channel, 0, (ultraled_rgb_t) {.green = 255});
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Channel %u test: pixel 1 green (GPIO %d, brightness %u%%)",
                     (unsigned)channel + 1U, config->channels[channel].gpio_num,
                     config->channels[channel].brightness_percent);
            if (config->channels[channel].brightness_percent == 0) {
                ESP_LOGW(TAG, "Channel %u test will not be visible because brightness is 0%%",
                         (unsigned)channel + 1U);
            }
        }
    }
    if (err == ESP_OK) {
        err = ultraled_show(s_handle, ULTRALED_WAIT_FOREVER);
    }
    if (err == ESP_OK) {
        return ESP_OK;
    }

fail:
    ultraled_del(s_handle);
    s_handle = NULL;
    return err;
}

esp_err_t ultraled_manager_init(void)
{
    esp_err_t gpio_error = board_hal_validate_led_gpios();
    if (gpio_error != ESP_OK) {
        ESP_LOGE(TAG, "Board LED GPIO configuration is invalid");
        s_last_error = gpio_error;
        s_state = ULTRALED_MANAGER_ERROR;
        return gpio_error;
    }

    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ultraled_manager_config_t config;
    esp_err_t err = read_nvs_config(&config);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_config(&config);
        err = write_nvs_config(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store default configuration: %s", esp_err_to_name(err));
            s_config = config;
            s_state = ULTRALED_MANAGER_ERROR;
            s_last_error = err;
            return err;
        }
        ESP_LOGI(TAG, "Stored safe disabled default configuration");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid stored configuration: %s", esp_err_to_name(err));
        set_default_config(&config);
        s_config = config;
        s_state = ULTRALED_MANAGER_ERROR;
        s_last_error = err;
        return err;
    }

    s_config = config;
    if (!config.enabled) {
        s_state = ULTRALED_MANAGER_DISABLED;
        s_last_error = ESP_OK;
        ESP_LOGI(TAG, "UltraLED is disabled");
        return ESP_OK;
    }

    err = start_ultraled(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UltraLED: %s", esp_err_to_name(err));
        s_state = ULTRALED_MANAGER_ERROR;
        s_last_error = err;
        return err;
    }

    s_state = ULTRALED_MANAGER_ACTIVE;
    s_last_error = ESP_OK;
    ESP_LOGI(TAG, "UltraLED started with %u channel(s)", config.channel_count);
    return ESP_OK;
}

void ultraled_manager_get_config(ultraled_manager_config_t *config)
{
    if (config == NULL) {
        return;
    }

    if (s_lock == NULL) {
        set_default_config(config);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_lock);
}

ultraled_manager_state_t ultraled_manager_get_state(void)
{
    return s_state;
}

esp_err_t ultraled_manager_get_last_error(void)
{
    return s_last_error;
}

ultraled_handle_t ultraled_manager_get_handle(void)
{
    return s_handle;
}
