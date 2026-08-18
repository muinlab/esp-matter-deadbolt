#include "door_controller.h"
#include "hal_gpio.h"
#include "comm_layer.h"
#include "status_led.h"
#include "diagnostics.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "door_ctrl";

static bool g_lock_state = true;
static SemaphoreHandle_t s_door_mutex = NULL;
static esp_timer_handle_t s_exit_timer = NULL;

/* ── Command Queue ── */
typedef struct {
    bool target_unlock;
    uint32_t sequence;
} door_cmd_t;
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t  s_worker_task = NULL;

/* ── Forward ── */
static void exit_timer_cb(void *arg);
static op_result_t door_execute_tracked(bool target_unlock,
                                        diagnostics_command_type_t type,
                                        diagnostics_command_origin_t origin,
                                        uint32_t sequence);

/* ═══════════════════════════════════════════════════════════
 *  도어 제어
 *  해제: GPIO HIGH (지속) → 릴레이 ON
 *  잠금: GPIO LOW → 릴레이 OFF
 * ═══════════════════════════════════════════════════════════ */

static op_result_t door_execute_tracked(bool target_unlock,
                                        diagnostics_command_type_t type,
                                        diagnostics_command_origin_t origin,
                                        uint32_t sequence)
{
    if (sequence == 0) {
        sequence = diagnostics_note_command_received(type, origin, target_unlock);
    }
    diagnostics_note_command_started(sequence);

    if (xSemaphoreTakeRecursive(s_door_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "동작 진행 중 → 명령 거부");
        diagnostics_note_command_completed(sequence, DIAG_RESULT_BUSY);
        return OP_RESULT_FAIL_BUSY;
    }

    if (target_unlock) {
        status_led_notify_unlocking();
        deadbolt_unlock();
        g_lock_state = false;
        diagnostics_note_relay_commanded(sequence);
        report_result(OP_RESULT_SUCCESS, 1);
        status_led_set_locked(false);
        ESP_LOGI(TAG, "해제 완료");
    } else {
        status_led_notify_locking();
        deadbolt_lock();
        g_lock_state = true;
        diagnostics_note_relay_commanded(sequence);
        report_result(OP_RESULT_SUCCESS, 1);
        status_led_set_locked(true);
        ESP_LOGI(TAG, "잠금 완료");
    }

    xSemaphoreGiveRecursive(s_door_mutex);
    diagnostics_note_command_completed(sequence, DIAG_RESULT_SUCCESS);
    return OP_RESULT_SUCCESS;
}

op_result_t door_execute(bool target_unlock)
{
    return door_execute_tracked(target_unlock,
                                target_unlock ? DIAG_COMMAND_UNLOCK : DIAG_COMMAND_LOCK,
                                DIAG_COMMAND_ORIGIN_INTERNAL,
                                0);
}

/* ── 퇴실 기능 ── */

static void exit_lock_task(void *arg)
{
    ESP_LOGI(TAG, "퇴실 시간 만료 → 자동 잠금");
    door_execute_tracked(false, DIAG_COMMAND_AUTO_LOCK, DIAG_COMMAND_ORIGIN_TIMER, 0);
    vTaskDelete(NULL);
}

static void exit_timer_cb(void *arg)
{
    xTaskCreate(exit_lock_task, "exit_lock", 4096, NULL, 5, NULL);
}

esp_err_t door_exit_open(uint8_t duration_sec)
{
    if (duration_sec < EXIT_MIN_SEC || duration_sec > EXIT_MAX_SEC)
        duration_sec = DEFAULT_EXIT_DURATION_SEC;

    uint32_t sequence = diagnostics_note_command_received(
        DIAG_COMMAND_EXIT_OPEN, DIAG_COMMAND_ORIGIN_MATTER, true);
    op_result_t result = door_execute_tracked(true, DIAG_COMMAND_EXIT_OPEN,
                                              DIAG_COMMAND_ORIGIN_MATTER, sequence);
    if (result != OP_RESULT_SUCCESS)
        return ESP_FAIL;

    esp_timer_stop(s_exit_timer);
    esp_timer_start_once(s_exit_timer, (uint64_t)duration_sec * 1000000ULL);
    ESP_LOGI(TAG, "퇴실: %d초 후 자동 잠금", duration_sec);
    return ESP_OK;
}

/* ── 명령 큐 ── */

static void door_worker_task(void *arg)
{
    door_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "큐 실행: %s", cmd.target_unlock ? "해제" : "잠금");
            door_execute_tracked(cmd.target_unlock,
                                 cmd.target_unlock ? DIAG_COMMAND_UNLOCK : DIAG_COMMAND_LOCK,
                                 DIAG_COMMAND_ORIGIN_MATTER,
                                 cmd.sequence);
        }
    }
}

void door_queue_command(bool target_unlock)
{
    uint32_t sequence = diagnostics_note_command_received(
        target_unlock ? DIAG_COMMAND_UNLOCK : DIAG_COMMAND_LOCK,
        DIAG_COMMAND_ORIGIN_MATTER,
        target_unlock);
    door_cmd_t cmd = { .target_unlock = target_unlock, .sequence = sequence };

    if (s_cmd_queue) {
        /* 진단용 best-effort 관측이다. 원래 xQueueOverwrite 스케줄링은 변경하지 않는다. */
        door_cmd_t previous = {};
        if (xQueuePeek(s_cmd_queue, &previous, 0) == pdTRUE) {
            diagnostics_note_queue_overwrite_observed(previous.sequence);
        }
        xQueueOverwrite(s_cmd_queue, &cmd);
        diagnostics_note_command_queued(sequence);
        ESP_LOGI(TAG, "큐 등록: %s", target_unlock ? "해제" : "잠금");
    } else {
        diagnostics_note_queue_unavailable(sequence);
    }
}

/* ── 초기화 ── */

esp_err_t door_controller_init(void)
{
    s_door_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_door_mutex) return ESP_FAIL;

    esp_timer_create_args_t exit_args = {
        .callback = exit_timer_cb, .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK, .name = "exit_lock",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&exit_args, &s_exit_timer), TAG, "타이머 실패");

    s_cmd_queue = xQueueCreate(1, sizeof(door_cmd_t));
    if (!s_cmd_queue) return ESP_FAIL;

    if (xTaskCreate(door_worker_task, "door_worker", 4096, NULL, 5, &s_worker_task) != pdPASS)
        return ESP_FAIL;

    ESP_LOGI(TAG, "도어 컨트롤러 초기화 완료");
    return ESP_OK;
}

bool door_is_locked(void) { return g_lock_state; }
