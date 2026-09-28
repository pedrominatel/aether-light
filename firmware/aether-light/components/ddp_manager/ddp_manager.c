#include "ddp_manager.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "nvs.h"
#include "ultraled_manager.h"

#define DDP_MANAGER_NVS_NAMESPACE "ddp_mgr"
#define DDP_MANAGER_NVS_KEY        "config"
#define DDP_MANAGER_NVS_MAGIC      0x44445043U
#define DDP_MANAGER_NVS_VERSION    1U
#define DDP_OUTPUT_TASK_STACK_SIZE 4096U
#define DDP_OUTPUT_TASK_PRIORITY   5U
#define DDP_OUTPUT_TIMEOUT_MS      100
#define DDP_OUTPUT_BACKOFF_MIN_MS  100U
#define DDP_OUTPUT_BACKOFF_MAX_MS  1000U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t enabled;
    uint8_t transports;
    uint8_t frame_policy;
    uint8_t reserved;
    uint32_t start_channel;
    uint16_t frame_timeout_ms;
    uint16_t source_timeout_ms;
} ddp_manager_nvs_v1_t;

_Static_assert(sizeof(ddp_manager_nvs_v1_t) == 20, "Unexpected DDP NVS layout");
_Static_assert(sizeof(ultraled_rgb_t) == 3, "DDP requires packed RGB pixels");

static const char *TAG = "ddp_manager";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_monitor_task;
static TaskHandle_t s_output_task;
static ddp_server_handle_t s_server;
static ddp_manager_config_t s_config;
static ddp_manager_config_t s_runtime_config;
static ddp_manager_status_t s_status;
static ultraled_manager_config_t s_led_config;
static uint8_t *s_frame;
static uint8_t *s_pending_frame;
static uint8_t *s_output_frame;
static uint8_t *s_coverage;
static size_t s_frame_length;
static size_t s_coverage_length;
static size_t s_covered_bytes;
static bool s_frame_open;
static bool s_pending_frame_ready;
static uint32_t s_active_source;
static TickType_t s_last_source_tick;
static TickType_t s_frame_started_tick;

static void set_default_config(ddp_manager_config_t *config)
{
    *config = (ddp_manager_config_t) {
        .enabled = false,
        .transports = DDP_TRANSPORT_UDP,
        .start_channel = 1,
        .frame_policy = DDP_MANAGER_FRAME_XLIGHTS,
        .frame_timeout_ms = 1000,
        .source_timeout_ms = 2500,
    };
}

esp_err_t ddp_manager_validate_config(const ddp_manager_config_t *config)
{
    if (config == NULL ||
        (config->transports & ~(DDP_TRANSPORT_UDP | DDP_TRANSPORT_TCP)) != 0 ||
        config->transports == 0 || config->start_channel == 0 ||
        config->start_channel > UINT32_MAX -
                                    (ULTRALED_MAX_CHANNELS * ULTRALED_MANAGER_MAX_PIXELS_PER_CHANNEL * 3U) ||
        config->frame_policy > DDP_MANAGER_FRAME_PARTIAL ||
        config->frame_timeout_ms < 50 || config->frame_timeout_ms > 5000 ||
        config->source_timeout_ms < 100 || config->source_timeout_ms > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void encode_nvs(const ddp_manager_config_t *config, ddp_manager_nvs_v1_t *stored)
{
    *stored = (ddp_manager_nvs_v1_t) {
        .magic = DDP_MANAGER_NVS_MAGIC,
        .version = DDP_MANAGER_NVS_VERSION,
        .size = sizeof(*stored),
        .enabled = config->enabled ? 1 : 0,
        .transports = config->transports,
        .frame_policy = (uint8_t)config->frame_policy,
        .start_channel = config->start_channel,
        .frame_timeout_ms = config->frame_timeout_ms,
        .source_timeout_ms = config->source_timeout_ms,
    };
}

static esp_err_t decode_nvs(const ddp_manager_nvs_v1_t *stored, ddp_manager_config_t *config)
{
    if (stored->magic != DDP_MANAGER_NVS_MAGIC || stored->version != DDP_MANAGER_NVS_VERSION ||
        stored->size != sizeof(*stored) || stored->enabled > 1) {
        return ESP_ERR_INVALID_VERSION;
    }
    *config = (ddp_manager_config_t) {
        .enabled = stored->enabled != 0,
        .transports = stored->transports,
        .start_channel = stored->start_channel,
        .frame_policy = (ddp_manager_frame_policy_t)stored->frame_policy,
        .frame_timeout_ms = stored->frame_timeout_ms,
        .source_timeout_ms = stored->source_timeout_ms,
    };
    return ddp_manager_validate_config(config);
}

static esp_err_t write_nvs(const ddp_manager_config_t *config)
{
    ddp_manager_nvs_v1_t stored;
    encode_nvs(config, &stored);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DDP_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, DDP_MANAGER_NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t read_nvs(ddp_manager_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DDP_MANAGER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    ddp_manager_nvs_v1_t stored = {0};
    size_t length = sizeof(stored);
    err = nvs_get_blob(handle, DDP_MANAGER_NVS_KEY, &stored, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(stored)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return decode_nvs(&stored, config);
}

esp_err_t ddp_manager_save_config(const ddp_manager_config_t *config)
{
    esp_err_t err = ddp_manager_validate_config(config);
    if (err != ESP_OK) {
        return err;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = write_nvs(config);
    if (err == ESP_OK) {
        s_config = *config;
        ESP_LOGI(TAG, "Saved DDP configuration: enabled=%u transports=0x%02x "
                      "start=%" PRIu32 " policy=%u frame_timeout=%u source_timeout=%u; restart required",
                 config->enabled, config->transports, config->start_channel,
                 config->frame_policy, config->frame_timeout_ms, config->source_timeout_ms);
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void reset_frame_locked(void)
{
    if (s_coverage != NULL) {
        memset(s_coverage, 0, s_coverage_length);
    }
    s_covered_bytes = 0;
    s_frame_open = false;
}

static bool take_source_locked(uint32_t source, TickType_t now)
{
    TickType_t timeout = pdMS_TO_TICKS(s_runtime_config.source_timeout_ms);
    if (s_active_source == 0 || s_active_source == source ||
        (now - s_last_source_tick) >= timeout) {
        if (s_active_source != source) {
            ESP_LOGI(TAG, "Controller ownership moved to " IPSTR,
                     IP2STR((esp_ip4_addr_t *)&source));
            reset_frame_locked();
            s_active_source = source;
        }
        s_last_source_tick = now;
        s_status.last_source_ipv4 = source;
        return true;
    }
    s_status.rejected_sources++;
    if (s_status.rejected_sources == 1 || s_status.rejected_sources % 100 == 0) {
        ESP_LOGW(TAG, "Rejected controller " IPSTR " while " IPSTR
                      " owns output (%" PRIu64 " rejected packets)",
                 IP2STR((esp_ip4_addr_t *)&source),
                 IP2STR((esp_ip4_addr_t *)&s_active_source), s_status.rejected_sources);
    }
    return false;
}

static void mark_coverage_locked(size_t offset, size_t length)
{
    for (size_t index = offset; index < offset + length; ++index) {
        uint8_t mask = (uint8_t)(1U << (index & 7U));
        uint8_t *byte = &s_coverage[index >> 3];
        if ((*byte & mask) == 0) {
            *byte |= mask;
            s_covered_bytes++;
        }
    }
}

static esp_err_t data_callback(void *context, const ddp_packet_t *packet)
{
    (void)context;
    if (packet->destination_id != DDP_DISPLAY_ID && packet->destination_id != DDP_ALL_ID) {
        ESP_LOGV(TAG, "Ignoring data for destination %u", packet->destination_id);
        return ESP_OK;
    }
    if ((packet->flags & DDP_FLAG_STORAGE) != 0 ||
        (packet->data_type != DDP_TYPE_UNDEFINED &&
         packet->data_type != DDP_TYPE_LEGACY_RGB && packet->data_type != DDP_TYPE_RGB24)) {
        ESP_LOGD(TAG, "Unsupported display packet: flags=0x%02x type=0x%02x",
                 packet->flags, packet->data_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t local_start = (uint64_t)s_runtime_config.start_channel - 1U;
    uint64_t local_end = local_start + s_frame_length;
    uint64_t packet_start = packet->offset;
    uint64_t packet_end = packet_start + packet->data_length;
    if (packet_end <= local_start || packet_start >= local_end || packet->data_length == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    if (!take_source_locked(packet->source_ipv4, now)) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    bool contains_first_byte = packet_start <= local_start && packet_end > local_start;
    if (s_runtime_config.frame_policy == DDP_MANAGER_FRAME_XLIGHTS) {
        if (!s_frame_open) {
            if (!contains_first_byte) {
                xSemaphoreGive(s_lock);
                return ESP_OK;
            }
            reset_frame_locked();
            s_frame_open = true;
            s_frame_started_tick = now;
            ESP_LOGD(TAG, "Started complete frame from " IPSTR,
                     IP2STR((esp_ip4_addr_t *)&packet->source_ipv4));
        } else if (contains_first_byte && s_covered_bytes > 0) {
            ESP_LOGD(TAG, "New frame start replaced an unpushed frame (%u/%u bytes covered)",
                     (unsigned)s_covered_bytes, (unsigned)s_frame_length);
            reset_frame_locked();
            s_frame_open = true;
            s_frame_started_tick = now;
        } else if ((now - s_frame_started_tick) >= pdMS_TO_TICKS(s_runtime_config.frame_timeout_ms)) {
            ESP_LOGW(TAG, "Frame assembly timed out after %u ms (%u/%u bytes covered)",
                     s_runtime_config.frame_timeout_ms, (unsigned)s_covered_bytes,
                     (unsigned)s_frame_length);
            reset_frame_locked();
            if (!contains_first_byte) {
                xSemaphoreGive(s_lock);
                return ESP_OK;
            }
            s_frame_open = true;
            s_frame_started_tick = now;
        }
    } else if (!s_frame_open) {
        s_frame_open = true;
        s_frame_started_tick = now;
    }

    uint64_t copy_start = packet_start > local_start ? packet_start : local_start;
    uint64_t copy_end = packet_end < local_end ? packet_end : local_end;
    size_t destination_offset = (size_t)(copy_start - local_start);
    size_t source_offset = (size_t)(copy_start - packet_start);
    size_t copy_length = (size_t)(copy_end - copy_start);
    memcpy(s_frame + destination_offset, packet->data + source_offset, copy_length);
    if (s_runtime_config.frame_policy == DDP_MANAGER_FRAME_XLIGHTS) {
        mark_coverage_locked(destination_offset, copy_length);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static bool take_pending_frame(void)
{
    bool available = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_frame_ready) {
        uint8_t *previous_output = s_output_frame;
        s_output_frame = s_pending_frame;
        s_pending_frame = previous_output;
        s_pending_frame_ready = false;
        available = true;
    }
    xSemaphoreGive(s_lock);
    return available;
}

static esp_err_t output_frame(const uint8_t *frame)
{
    ultraled_handle_t handle = ultraled_manager_get_handle();
    if (handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    size_t byte_offset = 0;
    for (size_t channel = 0; channel < s_led_config.channel_count; ++channel) {
        size_t count = s_led_config.channels[channel].led_count;
        err = ultraled_set_pixels(handle, channel, 0,
                                  (const ultraled_rgb_t *)(frame + byte_offset), count);
        if (err != ESP_OK) {
            return err;
        }
        byte_offset += count * sizeof(ultraled_rgb_t);
    }
    return ultraled_show(handle, DDP_OUTPUT_TIMEOUT_MS);
}

static void output_task(void *argument)
{
    (void)argument;
    uint32_t backoff_ms = DDP_OUTPUT_BACKOFF_MIN_MS;
    uint32_t consecutive_errors = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!take_pending_frame()) {
            continue;
        }

        esp_err_t err = output_frame(s_output_frame);
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint64_t displayed = ++s_status.displayed_frames;
            xSemaphoreGive(s_lock);

            if (consecutive_errors != 0) {
                ESP_LOGI(TAG, "DDP LED output recovered after %" PRIu32 " error(s)",
                         consecutive_errors);
            }
            consecutive_errors = 0;
            backoff_ms = DDP_OUTPUT_BACKOFF_MIN_MS;
            if (displayed == 1 || displayed % 1000 == 0) {
                ESP_LOGI(TAG, "Displayed DDP frame %" PRIu64, displayed);
            }
            continue;
        }

        consecutive_errors++;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint64_t errors = ++s_status.output_errors;
        s_status.last_output_error = err;
        xSemaphoreGive(s_lock);
        if (errors == 1 || errors % 10 == 0) {
            ESP_LOGW(TAG, "DDP LED output failed: %s (%" PRIu64
                          " errors, retry backoff %" PRIu32 " ms)",
                     esp_err_to_name(err), errors, backoff_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < DDP_OUTPUT_BACKOFF_MAX_MS) {
            backoff_ms *= 2U;
            if (backoff_ms > DDP_OUTPUT_BACKOFF_MAX_MS) {
                backoff_ms = DDP_OUTPUT_BACKOFF_MAX_MS;
            }
        }
    }
}

static esp_err_t push_callback(void *context, const ddp_packet_t *packet)
{
    (void)context;
    if (packet->destination_id != DDP_DISPLAY_ID && packet->destination_id != DDP_ALL_ID) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    if (!take_source_locked(packet->source_ipv4, now) || !s_frame_open) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    bool notify_output = false;
    if (s_runtime_config.frame_policy == DDP_MANAGER_FRAME_XLIGHTS && s_covered_bytes != s_frame_length) {
        s_status.incomplete_frames++;
        if (s_status.incomplete_frames == 1 || s_status.incomplete_frames % 100 == 0) {
            ESP_LOGW(TAG, "Discarded incomplete DDP frame: %u/%u bytes "
                          "(%" PRIu64 " incomplete frames)",
                     (unsigned)s_covered_bytes, (unsigned)s_frame_length,
                     s_status.incomplete_frames);
        }
    } else {
        ESP_LOGD(TAG, "Push accepted: %u frame bytes, policy=%u",
                 (unsigned)s_frame_length, s_runtime_config.frame_policy);
        if (s_pending_frame == NULL || s_output_task == NULL) {
            s_status.output_errors++;
            s_status.last_output_error = ESP_ERR_INVALID_STATE;
            reset_frame_locked();
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_pending_frame_ready) {
            s_status.superseded_frames++;
        }
        memcpy(s_pending_frame, s_frame, s_frame_length);
        s_pending_frame_ready = true;
        notify_output = true;
    }
    reset_frame_locked();
    TaskHandle_t output = s_output_task;
    xSemaphoreGive(s_lock);
    if (notify_output && output != NULL) {
        xTaskNotifyGive(output);
    }
    return ESP_OK;
}

static esp_err_t query_callback(void *context, const ddp_packet_t *query,
                                uint8_t *response, size_t capacity, size_t *response_length)
{
    (void)context;
    if (response == NULL || response_length == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int length = -1;
    if (query->destination_id == DDP_STATUS_ID) {
        ESP_LOGD(TAG, "Building DDP status response");
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_ETH);
        const esp_app_desc_t *app = esp_app_get_description();
        length = snprintf((char *)response, capacity,
                          "{\"status\":{\"man\":\"Aether Light\",\"mod\":\"ESP32-P4 UltraLED\","
                          "\"ver\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                          "\"push\":true,\"ntp\":false}}",
                          app->version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else if (query->destination_id == DDP_CONFIG_ID) {
        ESP_LOGD(TAG, "Building DDP configuration response for %u LED channel(s)",
                 s_led_config.channel_count);
        length = snprintf((char *)response, capacity, "{\"config\":{\"ports\":[");
        if (length < 0 || (size_t)length >= capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t used = (size_t)length;
        for (size_t channel = 0; channel < s_led_config.channel_count; ++channel) {
            int written = snprintf((char *)response + used, capacity - used,
                                   "%s{\"port\":%u,\"ts\":\"0\",\"l\":\"%u\",\"ss\":\"1\"}",
                                   channel == 0 ? "" : ",", (unsigned)channel,
                                   (unsigned)s_led_config.channels[channel].led_count);
            if (written < 0 || (size_t)written >= capacity - used) {
                return ESP_ERR_INVALID_SIZE;
            }
            used += (size_t)written;
        }
        int written = snprintf((char *)response + used, capacity - used, "]}}");
        if (written < 0 || (size_t)written >= capacity - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        length = (int)(used + (size_t)written);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (length < 0 || (size_t)length >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_length = (size_t)length;
    return ESP_OK;
}

static bool network_ready(void)
{
    network_manager_state_t state = network_manager_get_state();
    return state == NETWORK_MANAGER_CONNECTED_ETH || state == NETWORK_MANAGER_CONNECTED_WIFI;
}

static esp_err_t start_server(void)
{
    ddp_server_config_t server_config = DDP_SERVER_CONFIG_DEFAULT();
    server_config.transports = s_runtime_config.transports;
    ddp_callbacks_t callbacks = {
        .on_data = data_callback,
        .on_push = push_callback,
        .on_query = query_callback,
    };
    ESP_LOGI(TAG, "Starting server: port=%u transports=0x%02x start channel=%" PRIu32
                  " policy=%u output=%u bytes",
             server_config.port, server_config.transports, s_runtime_config.start_channel,
             s_runtime_config.frame_policy, (unsigned)s_frame_length);
    ddp_server_handle_t server = NULL;
    esp_err_t err = ddp_server_start(&server_config, &callbacks, NULL, &server);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        s_server = server;
    }
    s_status.state = err == ESP_OK ? DDP_MANAGER_RUNNING : DDP_MANAGER_ERROR;
    s_status.last_error = err;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DDP server: %s", esp_err_to_name(err));
    }
    return err;
}

static void stop_server(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ddp_server_handle_t server = s_server;
    s_server = NULL;
    xSemaphoreGive(s_lock);
    if (server != NULL) {
        esp_err_t err = ddp_server_stop(server);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop DDP server: %s", esp_err_to_name(err));
            return;
        }
        s_server = NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = s_runtime_config.enabled ? DDP_MANAGER_WAITING_NETWORK : DDP_MANAGER_DISABLED;
    reset_frame_locked();
    s_pending_frame_ready = false;
    xSemaphoreGive(s_lock);
}

static void monitor_task(void *argument)
{
    (void)argument;
    bool was_ready = false;
    while (true) {
        bool ready = network_ready();
        if (ready && !was_ready) {
            ESP_LOGI(TAG, "Network is ready; enabling DDP listener");
            if (start_server() == ESP_OK) {
                was_ready = true;
            }
        } else if (!ready && was_ready) {
            ESP_LOGW(TAG, "Network is unavailable; stopping DDP listener");
            stop_server();
            was_ready = false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t ddp_manager_init(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = read_nvs(&s_config);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_config(&s_config);
        err = write_nvs(&s_config);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "Stored disabled default DDP configuration");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid stored DDP configuration: %s", esp_err_to_name(err));
        set_default_config(&s_config);
        s_status.state = DDP_MANAGER_ERROR;
        s_status.last_error = err;
        return err;
    }

    ultraled_manager_get_config(&s_led_config);
    s_runtime_config = s_config;
    for (size_t channel = 0; channel < s_led_config.channel_count; ++channel) {
        s_frame_length += s_led_config.channels[channel].led_count * sizeof(ultraled_rgb_t);
    }
    s_coverage_length = (s_frame_length + 7U) / 8U;
    s_frame = calloc(s_frame_length, 1);
    if (s_runtime_config.enabled) {
        s_pending_frame = calloc(s_frame_length, 1);
        s_output_frame = calloc(s_frame_length, 1);
    }
    s_coverage = calloc(s_coverage_length, 1);
    if (s_frame == NULL || s_coverage == NULL ||
        (s_runtime_config.enabled && (s_pending_frame == NULL || s_output_frame == NULL))) {
        free(s_frame);
        free(s_pending_frame);
        free(s_output_frame);
        free(s_coverage);
        s_frame = NULL;
        s_pending_frame = NULL;
        s_output_frame = NULL;
        s_coverage = NULL;
        s_status.state = DDP_MANAGER_ERROR;
        s_status.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    s_status.output_bytes = s_frame_length;
    s_status.state = s_runtime_config.enabled ? DDP_MANAGER_WAITING_NETWORK : DDP_MANAGER_DISABLED;
    s_status.last_error = ESP_OK;
    ESP_LOGI(TAG, "DDP %s: transports=0x%02x start=%" PRIu32 " policy=%u "
                  "frame_timeout=%u source_timeout=%u output=%" PRIu32
                  " bytes, framebuffers=%u bytes, coverage=%u bytes, no PSRAM required",
             s_runtime_config.enabled ? "enabled" : "disabled", s_runtime_config.transports,
             s_runtime_config.start_channel, s_runtime_config.frame_policy,
             s_runtime_config.frame_timeout_ms, s_runtime_config.source_timeout_ms,
             s_status.output_bytes,
             (unsigned)(s_frame_length * (s_runtime_config.enabled ? 3U : 1U)),
             (unsigned)s_coverage_length);
    return ESP_OK;
}

void ddp_manager_get_config(ddp_manager_config_t *config)
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

void ddp_manager_get_status(ddp_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    if (s_server != NULL) {
        ddp_server_get_stats(s_server, &status->protocol);
    }
    xSemaphoreGive(s_lock);
}

void ddp_manager_start_when_network_ready(void)
{
    if (!s_runtime_config.enabled || s_monitor_task != NULL) {
        return;
    }

    if (xTaskCreate(output_task, "ddp_output", DDP_OUTPUT_TASK_STACK_SIZE, NULL,
                    DDP_OUTPUT_TASK_PRIORITY, &s_output_task) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DDP_MANAGER_ERROR;
        s_status.last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "Failed to create DDP output task");
        return;
    }

    if (xTaskCreate(monitor_task, "ddp_manager", 4096, NULL, 5, &s_monitor_task) != pdPASS) {
        vTaskDelete(s_output_task);
        s_output_task = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = DDP_MANAGER_ERROR;
        s_status.last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "Failed to create DDP manager task");
    }
}
