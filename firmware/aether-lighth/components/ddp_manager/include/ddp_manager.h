#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ddp.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DDP_MANAGER_FRAME_XLIGHTS = 0,
    DDP_MANAGER_FRAME_PARTIAL,
} ddp_manager_frame_policy_t;

typedef struct {
    bool enabled;
    uint8_t transports;
    uint32_t start_channel;
    ddp_manager_frame_policy_t frame_policy;
    uint16_t frame_timeout_ms;
    uint16_t source_timeout_ms;
} ddp_manager_config_t;

typedef enum {
    DDP_MANAGER_DISABLED = 0,
    DDP_MANAGER_WAITING_NETWORK,
    DDP_MANAGER_RUNNING,
    DDP_MANAGER_ERROR,
} ddp_manager_state_t;

typedef struct {
    ddp_manager_state_t state;
    esp_err_t last_error;
    uint32_t last_source_ipv4;
    uint32_t output_bytes;
    uint64_t displayed_frames;
    uint64_t incomplete_frames;
    uint64_t busy_frames;
    uint64_t rejected_sources;
    ddp_server_stats_t protocol;
} ddp_manager_status_t;

esp_err_t ddp_manager_init(void);
esp_err_t ddp_manager_validate_config(const ddp_manager_config_t *config);
esp_err_t ddp_manager_save_config(const ddp_manager_config_t *config);
void ddp_manager_get_config(ddp_manager_config_t *config);
void ddp_manager_get_status(ddp_manager_status_t *status);
void ddp_manager_start_when_network_ready(void);

#ifdef __cplusplus
}
#endif
