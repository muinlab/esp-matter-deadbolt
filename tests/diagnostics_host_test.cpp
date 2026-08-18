#include <assert.h>

#include "diagnostics.h"

int64_t g_diagnostics_test_time_us = 0;

static void set_uptime_ms(uint64_t value)
{
    g_diagnostics_test_time_us = (int64_t)(value * 1000);
}

int main()
{
    diagnostics_init(ESP_RST_BROWNOUT);
    diagnostics_snapshot_t snapshot = {};
    diagnostics_get_snapshot(&snapshot);
    assert(snapshot.reset_reason == ESP_RST_BROWNOUT);
    assert(snapshot.last_wifi_disconnect_rssi == -128);

    set_uptime_ms(10);
    diagnostics_note_wifi_connected();
    set_uptime_ms(20);
    diagnostics_note_wifi_disconnected(200, -71);
    diagnostics_note_wifi_disconnected(0, -128);  // CHIP duplicate: same episode
    set_uptime_ms(30);
    diagnostics_note_wifi_connected();
    diagnostics_note_matter_server_ready();
    diagnostics_note_health_request();

    set_uptime_ms(100);
    uint32_t sequence = diagnostics_note_command_received(
        DIAG_COMMAND_UNLOCK, DIAG_COMMAND_ORIGIN_MATTER, true);
    set_uptime_ms(101);
    diagnostics_note_command_queued(sequence);
    set_uptime_ms(102);
    diagnostics_note_command_started(sequence);
    set_uptime_ms(103);
    diagnostics_note_relay_commanded(sequence);
    set_uptime_ms(106);
    diagnostics_note_command_completed(sequence, DIAG_RESULT_SUCCESS);

    diagnostics_get_snapshot(&snapshot);
    assert(snapshot.wifi_connected);
    assert(snapshot.wifi_connection_count == 2);
    assert(snapshot.wifi_disconnect_count == 1);
    assert(snapshot.wifi_reconnect_count == 1);
    assert(snapshot.last_wifi_disconnect_reason == 200);
    assert(snapshot.last_wifi_disconnect_rssi == -71);
    assert(snapshot.matter_server_ready);
    assert(snapshot.matter_application_command_callback_count == 1);
    assert(snapshot.health_request_count == 1);
    assert(snapshot.command_history_count == 1);
    assert(snapshot.commands[0].sequence == sequence);
    assert(snapshot.commands[0].stage == DIAG_STAGE_COMPLETED);
    assert(snapshot.commands[0].relay_commanded_uptime_ms == 103);

    /* A new episode with no raw reason/RSSI must not retain stale values. */
    set_uptime_ms(120);
    diagnostics_note_wifi_disconnected(0, -128);
    diagnostics_get_snapshot(&snapshot);
    assert(snapshot.wifi_disconnect_count == 2);
    assert(snapshot.last_wifi_disconnect_reason == 0);
    assert(snapshot.last_wifi_disconnect_rssi == -128);

    /* Producer telemetry arriving after a fast worker must not regress stage. */
    set_uptime_ms(150);
    uint32_t raced = diagnostics_note_command_received(
        DIAG_COMMAND_LOCK, DIAG_COMMAND_ORIGIN_MATTER, false);
    set_uptime_ms(151);
    diagnostics_note_command_started(raced);
    set_uptime_ms(152);
    diagnostics_note_command_queued(raced);
    diagnostics_get_snapshot(&snapshot);
    const diagnostics_command_record_t &raced_record =
        snapshot.commands[snapshot.command_history_count - 1];
    assert(raced_record.sequence == raced);
    assert(raced_record.stage == DIAG_STAGE_STARTED);
    assert(raced_record.queued_uptime_ms == 0);

    set_uptime_ms(160);
    uint32_t completed_race = diagnostics_note_command_received(
        DIAG_COMMAND_UNLOCK, DIAG_COMMAND_ORIGIN_MATTER, true);
    diagnostics_note_command_started(completed_race);
    diagnostics_note_relay_commanded(completed_race);
    diagnostics_note_command_completed(completed_race, DIAG_RESULT_SUCCESS);
    set_uptime_ms(161);
    diagnostics_note_command_queued(completed_race);
    diagnostics_get_snapshot(&snapshot);
    const diagnostics_command_record_t &completed_race_record =
        snapshot.commands[snapshot.command_history_count - 1];
    assert(completed_race_record.sequence == completed_race);
    assert(completed_race_record.stage == DIAG_STAGE_COMPLETED);
    assert(completed_race_record.queued_uptime_ms == 0);

    uint32_t newest = 0;
    for (int i = 0; i < 9; ++i) {
        set_uptime_ms(200 + i);
        newest = diagnostics_note_command_received(
            DIAG_COMMAND_LOCK, DIAG_COMMAND_ORIGIN_MATTER, false);
    }
    diagnostics_get_snapshot(&snapshot);
    assert(snapshot.command_history_count == DIAGNOSTICS_COMMAND_HISTORY_SIZE);
    assert(snapshot.commands[DIAGNOSTICS_COMMAND_HISTORY_SIZE - 1].sequence == newest);
    assert(snapshot.commands[0].sequence ==
           newest - DIAGNOSTICS_COMMAND_HISTORY_SIZE + 1);

    return 0;
}
