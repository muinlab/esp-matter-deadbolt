#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAGNOSTICS_COMMAND_HISTORY_SIZE 8

typedef enum {
    DIAG_COMMAND_NONE = 0,
    DIAG_COMMAND_LOCK,
    DIAG_COMMAND_UNLOCK,
    DIAG_COMMAND_EXIT_OPEN,
    DIAG_COMMAND_AUTO_LOCK,
} diagnostics_command_type_t;

typedef enum {
    DIAG_COMMAND_ORIGIN_INTERNAL = 0,
    DIAG_COMMAND_ORIGIN_MATTER,
    DIAG_COMMAND_ORIGIN_TIMER,
} diagnostics_command_origin_t;

typedef enum {
    DIAG_STAGE_NONE = 0,
    DIAG_STAGE_RECEIVED,
    DIAG_STAGE_QUEUED,
    DIAG_STAGE_STARTED,
    DIAG_STAGE_RELAY_COMMANDED,
    DIAG_STAGE_COMPLETED,
    DIAG_STAGE_REJECTED_BUSY,
    DIAG_STAGE_QUEUE_UNAVAILABLE,
} diagnostics_command_stage_t;

typedef enum {
    DIAG_RESULT_NONE = 0,
    DIAG_RESULT_SUCCESS,
    DIAG_RESULT_BUSY,
    DIAG_RESULT_INTERNAL_ERROR,
} diagnostics_command_result_t;

typedef struct {
    uint32_t sequence;
    diagnostics_command_type_t type;
    diagnostics_command_origin_t origin;
    diagnostics_command_stage_t stage;
    diagnostics_command_result_t result;
    bool target_unlock;
    uint64_t received_uptime_ms;
    uint64_t queued_uptime_ms;
    uint64_t started_uptime_ms;
    uint64_t relay_commanded_uptime_ms;
    uint64_t completed_uptime_ms;
} diagnostics_command_record_t;

typedef struct {
    esp_reset_reason_t reset_reason;
    bool wifi_connected;
    bool matter_server_ready;
    uint32_t wifi_connection_count;
    uint32_t wifi_disconnect_count;
    uint32_t wifi_reconnect_count;
    uint16_t last_wifi_disconnect_reason;
    int8_t last_wifi_disconnect_rssi;
    uint64_t last_wifi_disconnect_uptime_ms;
    uint64_t last_wifi_reconnect_uptime_ms;
    uint64_t matter_server_ready_uptime_ms;
    uint32_t matter_application_command_callback_count;
    uint32_t health_request_count;
    uint64_t last_health_request_uptime_ms;
    uint32_t command_received_count;
    uint32_t command_completed_count;
    uint32_t command_busy_count;
    uint32_t command_queue_overwrite_count;
    uint8_t command_history_count;
    diagnostics_command_record_t commands[DIAGNOSTICS_COMMAND_HISTORY_SIZE];
} diagnostics_snapshot_t;

void diagnostics_init(esp_reset_reason_t reset_reason);
void diagnostics_note_wifi_connected(void);
void diagnostics_note_wifi_disconnected(uint16_t reason, int8_t rssi);
void diagnostics_note_matter_server_ready(void);
void diagnostics_note_health_request(void);

uint32_t diagnostics_note_command_received(diagnostics_command_type_t type,
                                           diagnostics_command_origin_t origin,
                                           bool target_unlock);
void diagnostics_note_command_queued(uint32_t sequence);
void diagnostics_note_command_started(uint32_t sequence);
void diagnostics_note_relay_commanded(uint32_t sequence);
void diagnostics_note_command_completed(uint32_t sequence,
                                        diagnostics_command_result_t result);
void diagnostics_note_queue_overwrite_observed(uint32_t pending_sequence);
void diagnostics_note_queue_unavailable(uint32_t sequence);

void diagnostics_get_snapshot(diagnostics_snapshot_t *out);

const char *diagnostics_reset_reason_name(esp_reset_reason_t reason);
const char *diagnostics_command_type_name(diagnostics_command_type_t type);
const char *diagnostics_command_origin_name(diagnostics_command_origin_t origin);
const char *diagnostics_command_stage_name(diagnostics_command_stage_t stage);
const char *diagnostics_command_result_name(diagnostics_command_result_t result);

#ifdef __cplusplus
}
#endif
