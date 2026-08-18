#include "diagnostics.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "diagnostics";

typedef struct {
    diagnostics_snapshot_t public_state;
    bool wifi_ever_connected;
    bool wifi_loss_pending;
    uint32_t next_command_sequence;
    uint8_t command_head;
} diagnostics_state_t;

static diagnostics_state_t s_state = {};
static portMUX_TYPE s_diagnostics_mux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static diagnostics_command_record_t *find_command_locked(uint32_t sequence)
{
    for (uint8_t i = 0; i < DIAGNOSTICS_COMMAND_HISTORY_SIZE; ++i) {
        if (s_state.public_state.commands[i].sequence == sequence) {
            return &s_state.public_state.commands[i];
        }
    }
    return nullptr;
}

void diagnostics_init(esp_reset_reason_t reset_reason)
{
    portENTER_CRITICAL(&s_diagnostics_mux);
    memset(&s_state, 0, sizeof(s_state));
    s_state.public_state.reset_reason = reset_reason;
    s_state.public_state.last_wifi_disconnect_rssi = -128;
    portEXIT_CRITICAL(&s_diagnostics_mux);

    ESP_LOGI(TAG, "[DIAG] boot reset_reason=%s(%d)",
             diagnostics_reset_reason_name(reset_reason), (int)reset_reason);
}

void diagnostics_note_wifi_connected(void)
{
    bool reconnect = false;
    uint32_t connection_count;
    uint32_t reconnect_count;
    const uint64_t now = uptime_ms();

    portENTER_CRITICAL(&s_diagnostics_mux);
    if (!s_state.public_state.wifi_connected) {
        s_state.public_state.wifi_connection_count++;
    }
    reconnect = s_state.wifi_ever_connected && s_state.wifi_loss_pending;
    if (reconnect) {
        s_state.public_state.wifi_reconnect_count++;
        s_state.public_state.last_wifi_reconnect_uptime_ms = now;
    }
    s_state.public_state.wifi_connected = true;
    s_state.wifi_ever_connected = true;
    s_state.wifi_loss_pending = false;
    connection_count = s_state.public_state.wifi_connection_count;
    reconnect_count = s_state.public_state.wifi_reconnect_count;
    portEXIT_CRITICAL(&s_diagnostics_mux);

    ESP_LOGI(TAG, "[DIAG] wifi_connected connection_count=%lu reconnect_count=%lu reconnect=%d",
             (unsigned long)connection_count, (unsigned long)reconnect_count, reconnect);
}

void diagnostics_note_wifi_disconnected(uint16_t reason, int8_t rssi)
{
    uint32_t disconnect_count;
    bool new_episode;
    const uint64_t now = uptime_ms();

    portENTER_CRITICAL(&s_diagnostics_mux);
    new_episode = !s_state.wifi_loss_pending;
    s_state.public_state.wifi_connected = false;
    if (new_episode) {
        s_state.public_state.wifi_disconnect_count++;
        s_state.public_state.last_wifi_disconnect_reason = 0;
        s_state.public_state.last_wifi_disconnect_rssi = -128;
        s_state.public_state.last_wifi_disconnect_uptime_ms = now;
    }
    if (reason != 0 || s_state.public_state.last_wifi_disconnect_reason == 0) {
        s_state.public_state.last_wifi_disconnect_reason = reason;
    }
    if (rssi != -128 || s_state.public_state.last_wifi_disconnect_rssi == -128) {
        s_state.public_state.last_wifi_disconnect_rssi = rssi;
    }
    s_state.wifi_loss_pending = true;
    disconnect_count = s_state.public_state.wifi_disconnect_count;
    portEXIT_CRITICAL(&s_diagnostics_mux);

    ESP_LOGW(TAG,
             "[DIAG] wifi_disconnected disconnect_count=%lu reason=%u rssi=%d new_episode=%d uptime_ms=%llu",
             (unsigned long)disconnect_count, (unsigned int)reason, (int)rssi, new_episode,
             (unsigned long long)now);
}

void diagnostics_note_matter_server_ready(void)
{
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    s_state.public_state.matter_server_ready = true;
    s_state.public_state.matter_server_ready_uptime_ms = now;
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGI(TAG, "[DIAG] matter_server_ready uptime_ms=%llu", (unsigned long long)now);
}

void diagnostics_note_health_request(void)
{
    uint32_t count;
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    s_state.public_state.health_request_count++;
    s_state.public_state.last_health_request_uptime_ms = now;
    count = s_state.public_state.health_request_count;
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGI(TAG, "[DIAG] health_request count=%lu uptime_ms=%llu",
             (unsigned long)count, (unsigned long long)now);
}

uint32_t diagnostics_note_command_received(diagnostics_command_type_t type,
                                           diagnostics_command_origin_t origin,
                                           bool target_unlock)
{
    diagnostics_command_record_t record = {};
    uint32_t sequence;
    const uint64_t now = uptime_ms();

    portENTER_CRITICAL(&s_diagnostics_mux);
    sequence = ++s_state.next_command_sequence;
    if (sequence == 0) {
        sequence = ++s_state.next_command_sequence;
    }

    record.sequence = sequence;
    record.type = type;
    record.origin = origin;
    record.stage = DIAG_STAGE_RECEIVED;
    record.result = DIAG_RESULT_NONE;
    record.target_unlock = target_unlock;
    record.received_uptime_ms = now;

    s_state.public_state.commands[s_state.command_head] = record;
    s_state.command_head = (uint8_t)((s_state.command_head + 1) % DIAGNOSTICS_COMMAND_HISTORY_SIZE);
    if (s_state.public_state.command_history_count < DIAGNOSTICS_COMMAND_HISTORY_SIZE) {
        s_state.public_state.command_history_count++;
    }
    s_state.public_state.command_received_count++;
    if (origin == DIAG_COMMAND_ORIGIN_MATTER) {
        s_state.public_state.matter_application_command_callback_count++;
    }
    portEXIT_CRITICAL(&s_diagnostics_mux);

    ESP_LOGI(TAG, "[DIAG] command_received seq=%lu origin=%s type=%s target=%s",
             (unsigned long)sequence,
             diagnostics_command_origin_name(origin),
             diagnostics_command_type_name(type),
             target_unlock ? "unlocked" : "locked");
    return sequence;
}

void diagnostics_note_command_queued(uint32_t sequence)
{
    bool recorded = false;
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    diagnostics_command_record_t *record = find_command_locked(sequence);
    /* Worker가 먼저 실행된 경우 stage를 queued로 되돌리지 않는다. */
    if (record && record->stage == DIAG_STAGE_RECEIVED) {
        record->stage = DIAG_STAGE_QUEUED;
        record->queued_uptime_ms = now;
        recorded = true;
    }
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGI(TAG, "[DIAG] command_queued seq=%lu recorded=%d",
             (unsigned long)sequence, recorded);
}

void diagnostics_note_command_started(uint32_t sequence)
{
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    diagnostics_command_record_t *record = find_command_locked(sequence);
    if (record) {
        record->stage = DIAG_STAGE_STARTED;
        record->started_uptime_ms = now;
    }
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGI(TAG, "[DIAG] command_started seq=%lu", (unsigned long)sequence);
}

void diagnostics_note_relay_commanded(uint32_t sequence)
{
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    diagnostics_command_record_t *record = find_command_locked(sequence);
    if (record) {
        record->stage = DIAG_STAGE_RELAY_COMMANDED;
        record->relay_commanded_uptime_ms = now;
    }
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGI(TAG, "[DIAG] relay_commanded seq=%lu physical_bolt_verified=0",
             (unsigned long)sequence);
}

void diagnostics_note_command_completed(uint32_t sequence,
                                        diagnostics_command_result_t result)
{
    uint64_t duration_ms = 0;
    const uint64_t now = uptime_ms();

    portENTER_CRITICAL(&s_diagnostics_mux);
    diagnostics_command_record_t *record = find_command_locked(sequence);
    if (record) {
        record->result = result;
        record->completed_uptime_ms = now;
        if (record->started_uptime_ms > 0 && now >= record->started_uptime_ms) {
            duration_ms = now - record->started_uptime_ms;
        }
        if (result == DIAG_RESULT_BUSY) {
            record->stage = DIAG_STAGE_REJECTED_BUSY;
        } else {
            record->stage = DIAG_STAGE_COMPLETED;
        }
    }
    if (result == DIAG_RESULT_BUSY) {
        s_state.public_state.command_busy_count++;
    }
    s_state.public_state.command_completed_count++;
    portEXIT_CRITICAL(&s_diagnostics_mux);

    ESP_LOGI(TAG, "[DIAG] command_completed seq=%lu result=%s handler_ms=%llu",
             (unsigned long)sequence,
             diagnostics_command_result_name(result),
             (unsigned long long)duration_ms);
}

void diagnostics_note_queue_overwrite_observed(uint32_t pending_sequence)
{
    portENTER_CRITICAL(&s_diagnostics_mux);
    s_state.public_state.command_queue_overwrite_count++;
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGW(TAG, "[DIAG] queue_overwrite_observed pending_seq=%lu best_effort=1",
             (unsigned long)pending_sequence);
}

void diagnostics_note_queue_unavailable(uint32_t sequence)
{
    const uint64_t now = uptime_ms();
    portENTER_CRITICAL(&s_diagnostics_mux);
    diagnostics_command_record_t *record = find_command_locked(sequence);
    if (record) {
        record->stage = DIAG_STAGE_QUEUE_UNAVAILABLE;
        record->result = DIAG_RESULT_INTERNAL_ERROR;
        record->completed_uptime_ms = now;
    }
    portEXIT_CRITICAL(&s_diagnostics_mux);
    ESP_LOGE(TAG, "[DIAG] queue_unavailable seq=%lu", (unsigned long)sequence);
}

void diagnostics_get_snapshot(diagnostics_snapshot_t *out)
{
    if (!out) return;

    diagnostics_snapshot_t raw = {};
    uint8_t head;

    portENTER_CRITICAL(&s_diagnostics_mux);
    raw = s_state.public_state;
    head = s_state.command_head;
    portEXIT_CRITICAL(&s_diagnostics_mux);

    *out = raw;
    memset(out->commands, 0, sizeof(out->commands));

    const uint8_t count = raw.command_history_count;
    const uint8_t start = (count == DIAGNOSTICS_COMMAND_HISTORY_SIZE) ? head : 0;
    for (uint8_t i = 0; i < count; ++i) {
        out->commands[i] = raw.commands[(start + i) % DIAGNOSTICS_COMMAND_HISTORY_SIZE];
    }
}

const char *diagnostics_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "unknown";
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "other_watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        default:                return "other";
    }
}

const char *diagnostics_command_type_name(diagnostics_command_type_t type)
{
    switch (type) {
        case DIAG_COMMAND_LOCK:      return "lock";
        case DIAG_COMMAND_UNLOCK:    return "unlock";
        case DIAG_COMMAND_EXIT_OPEN: return "exit_open";
        case DIAG_COMMAND_AUTO_LOCK: return "auto_lock";
        default:                     return "none";
    }
}

const char *diagnostics_command_origin_name(diagnostics_command_origin_t origin)
{
    switch (origin) {
        case DIAG_COMMAND_ORIGIN_MATTER: return "matter";
        case DIAG_COMMAND_ORIGIN_TIMER:  return "timer";
        default:                         return "internal";
    }
}

const char *diagnostics_command_stage_name(diagnostics_command_stage_t stage)
{
    switch (stage) {
        case DIAG_STAGE_RECEIVED:          return "received";
        case DIAG_STAGE_QUEUED:            return "queued";
        case DIAG_STAGE_STARTED:           return "started";
        case DIAG_STAGE_RELAY_COMMANDED:   return "relay_commanded";
        case DIAG_STAGE_COMPLETED:         return "completed";
        case DIAG_STAGE_REJECTED_BUSY:     return "rejected_busy";
        case DIAG_STAGE_QUEUE_UNAVAILABLE: return "queue_unavailable";
        default:                           return "none";
    }
}

const char *diagnostics_command_result_name(diagnostics_command_result_t result)
{
    switch (result) {
        case DIAG_RESULT_SUCCESS:        return "success";
        case DIAG_RESULT_BUSY:           return "busy";
        case DIAG_RESULT_INTERNAL_ERROR: return "internal_error";
        default:                         return "none";
    }
}
