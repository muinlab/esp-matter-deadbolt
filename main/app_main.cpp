#include "hal_gpio.h"
#include "door_controller.h"
#include "comm_layer.h"
#include "status_led.h"
#include "diagnostics.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/Server.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DoorLock;

#if CONFIG_APP_RETRIEVE_LEN_ELF_SHA != 64
#error "RCA diagnostics require the full 64-character ELF SHA-256"
#endif

static const char *TAG = "app_main";

/* door_control_task는 더 이상 사용하지 않음 — door_queue_command() 사용 */

/* ── 커스텀 클러스터 ID ── */
static const uint32_t CUSTOM_CLUSTER_CONTROL       = 0x131BFC00;
static const uint32_t CUSTOM_ATTR_FACTORY_RESET    = 0x00000000;  // uint16, write 0xDEAD → 팩토리 리셋
static const uint32_t CUSTOM_ATTR_EXIT_OPEN        = 0x00000001;  // uint8, write duration(3~30) → 퇴실
static const uint32_t CUSTOM_ATTR_OTA_TRIGGER      = 0x00000002;  // uint8, write 1 → HTTPS OTA 시작
static const uint32_t CUSTOM_ATTR_HEALTH           = 0x00000003;  // uint32, write 1 → 헬스체크, read → 결과
static const uint16_t FACTORY_RESET_MAGIC          = 0xDEAD;

#define FIRMWARE_VERSION            "v1.1.5"
#define DIAGNOSTICS_REVISION        "edge76-rca1"

// GitHub Releases latest 고정 URL — 버전 업 시 URL 변경 불필요
#define OTA_FIRMWARE_URL \
    "https://github.com/muinlab/esp-matter-deadbolt/releases/latest/download/esp-matter-deadbolt.bin"

static void ota_task(void *arg);          // forward declaration

/* ── 내장 로그 HTTP 서버 ─────────────────────────────────── */

#define LOG_BUF_ENTRIES  60
#define LOG_ENTRY_LEN   192

struct log_entry_t {
    int64_t  ts_us;
    char     msg[LOG_ENTRY_LEN];
};

static log_entry_t      s_log_buf[LOG_BUF_ENTRIES] = {};
static volatile uint32_t s_log_head  = 0;  // 다음 쓸 인덱스
static volatile uint32_t s_log_count = 0;  // 저장된 총 개수 (최대 LOG_BUF_ENTRIES)
static portMUX_TYPE      s_log_mux   = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t    s_log_httpd = nullptr;

static int deadbolt_vprintf(const char *fmt, va_list args)
{
    /* 먼저 복사본으로 링 버퍼에 저장 */
    va_list copy;
    va_copy(copy, args);

    portENTER_CRITICAL(&s_log_mux);
    uint32_t idx = s_log_head % LOG_BUF_ENTRIES;
    s_log_buf[idx].ts_us = esp_timer_get_time();
    vsnprintf(s_log_buf[idx].msg, LOG_ENTRY_LEN, fmt, copy);
    /* 끝 개행 제거 */
    size_t l = strlen(s_log_buf[idx].msg);
    while (l > 0 && (s_log_buf[idx].msg[l - 1] == '\n' || s_log_buf[idx].msg[l - 1] == '\r'))
        s_log_buf[idx].msg[--l] = '\0';
    s_log_head++;
    if (s_log_count < LOG_BUF_ENTRIES) s_log_count++;
    portEXIT_CRITICAL(&s_log_mux);

    va_end(copy);
    return vprintf(fmt, args);  // UART 출력은 그대로
}

/* JSON 문자열 이스케이프 */
static void json_esc(char *out, size_t out_sz, const char *in)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 3 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"'; }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { /* skip */ }
        else if (c < 0x20)  { /* skip control chars */ }
        else                  out[j++] = (char)c;
    }
    out[j] = '\0';
}

static const char *LOG_HTML = R"rawhtml(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Deadbolt Log</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0d0d0d;color:#ddd;padding:20px}
h1{color:#7cf;margin-bottom:4px;font-size:20px}
.sub{color:#555;font-size:12px;margin-bottom:16px}
.st{color:#666;font-size:12px;margin-bottom:10px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{background:#1a1a1a;padding:7px 10px;text-align:left;color:#7cf;border-bottom:1px solid #333}
td{padding:5px 10px;border-bottom:1px solid #1a1a1a;vertical-align:top}
tr:hover td{background:#131313}
.ts{color:#555;white-space:nowrap}
.msg{word-break:break-all;color:#ccc}
.empty{color:#333;text-align:center;padding:40px}
</style>
</head>
<body>
<h1>Deadbolt Log</h1>
<div class="sub">ESP32 내장 로그 서버 · /logs 에서 JSON 조회 가능</div>
<div class="st" id="st">로딩 중...</div>
<table>
<thead><tr><th>업타임</th><th>로그</th></tr></thead>
<tbody id="tb"><tr><td colspan="2" class="empty">로그 없음</td></tr></tbody>
</table>
<script>
async function refresh(){
  try{
    const r=await fetch('/logs');
    const logs=await r.json();
    const tb=document.getElementById('tb');
    if(!logs.length){
      tb.innerHTML='<tr><td colspan="2" class="empty">로그 없음</td></tr>';
    } else {
      tb.innerHTML=[...logs].reverse().map(e=>{
        const s=Math.floor(e.ts/1000000);
        const up=s>=60?Math.floor(s/60)+'m '+(s%60)+'s':s+'s';
        return`<tr><td class="ts">${up}</td><td class="msg">${e.msg.replace(/</g,'&lt;')}</td></tr>`;
      }).join('');
    }
    document.getElementById('st').textContent=`총 ${logs.length}개 · ${new Date().toLocaleTimeString()} · 3초 자동갱신`;
  } catch(e){
    document.getElementById('st').textContent='응답 없음';
  }
}
refresh();setInterval(refresh,3000);
</script>
</body>
</html>)rawhtml";

static esp_err_t log_handler_html(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, LOG_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t log_handler_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    portENTER_CRITICAL(&s_log_mux);
    uint32_t count = s_log_count;
    uint32_t head  = s_log_head;
    portEXIT_CRITICAL(&s_log_mux);

    uint32_t start = (count >= LOG_BUF_ENTRIES) ? head : (head >= count ? head - count : 0);

    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (uint32_t i = 0; i < count && i < LOG_BUF_ENTRIES; i++) {
        uint32_t idx = (start + i) % LOG_BUF_ENTRIES;
        char esc[LOG_ENTRY_LEN * 2];
        json_esc(esc, sizeof(esc), s_log_buf[idx].msg);
        char chunk[LOG_ENTRY_LEN * 2 + 40];
        snprintf(chunk, sizeof(chunk), "%s{\"ts\":%lld,\"msg\":\"%s\"}",
                 first ? "" : ",",
                 (long long)s_log_buf[idx].ts_us,
                 esc);
        httpd_resp_sendstr_chunk(req, chunk);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t diagnostics_handler_json(httpd_req_t *req)
{
    diagnostics_snapshot_t snapshot = {};
    diagnostics_get_snapshot(&snapshot);

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    const uint32_t free_heap = esp_get_free_heap_size();
    const uint32_t minimum_free_heap = esp_get_minimum_free_heap_size();
    char firmware_build_id[65] = {};
    if (esp_app_get_elf_sha256(firmware_build_id, sizeof(firmware_build_id)) !=
        (int)sizeof(firmware_build_id)) {
        snprintf(firmware_build_id, sizeof(firmware_build_id), "unavailable");
    }
    wifi_ap_record_t ap_info = {};
    const bool wifi_driver_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    const int wifi_rssi = wifi_driver_connected ? (int)ap_info.rssi : -128;
    const unsigned int wifi_channel = wifi_driver_connected ? (unsigned int)ap_info.primary : 0;

    esp_err_t response_err = httpd_resp_set_type(req, "application/json");
    if (response_err != ESP_OK) return response_err;
    response_err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (response_err != ESP_OK) return response_err;
    response_err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (response_err != ESP_OK) return response_err;

    char chunk[1400];
    int length = snprintf(
        chunk, sizeof(chunk),
        "{\"schemaVersion\":1,\"firmwareVersion\":\"%s\","
        "\"diagnosticsRevision\":\"%s\",\"firmwareBuildId\":\"%s\","
        "\"autoOtaEnabled\":true,\"autoOtaChannel\":\"github-latest\",\"uptimeMs\":%llu,"
        "\"reset\":{\"code\":%d,\"reason\":\"%s\"},"
        "\"heap\":{\"freeBytes\":%lu,\"minimumFreeBytes\":%lu},"
        "\"wifi\":{\"eventConnected\":%s,\"driverConnected\":%s,"
        "\"rssiDbm\":%d,\"channel\":%u,\"connectionCount\":%lu,"
        "\"disconnectCount\":%lu,\"reconnectCount\":%lu,"
        "\"lastDisconnectReason\":%u,\"lastDisconnectRssiDbm\":%d,"
        "\"lastDisconnectUptimeMs\":%llu,\"lastReconnectUptimeMs\":%llu},"
        "\"matter\":{\"serverReady\":%s,\"serverReadyUptimeMs\":%llu,"
        "\"applicationCommandCallbackCount\":%lu,\"healthRequestCount\":%lu,"
        "\"lastHealthRequestUptimeMs\":%llu},"
        "\"command\":{\"receivedCount\":%lu,\"completedCount\":%lu,"
        "\"busyCount\":%lu,\"queueOverwriteObservedCount\":%lu,"
        "\"historyCapacity\":%u,\"physicalBoltVerificationAvailable\":false,\"recent\":[",
        FIRMWARE_VERSION,
        DIAGNOSTICS_REVISION,
        firmware_build_id,
        (unsigned long long)now_ms,
        (int)snapshot.reset_reason,
        diagnostics_reset_reason_name(snapshot.reset_reason),
        (unsigned long)free_heap,
        (unsigned long)minimum_free_heap,
        snapshot.wifi_connected ? "true" : "false",
        wifi_driver_connected ? "true" : "false",
        wifi_rssi,
        wifi_channel,
        (unsigned long)snapshot.wifi_connection_count,
        (unsigned long)snapshot.wifi_disconnect_count,
        (unsigned long)snapshot.wifi_reconnect_count,
        (unsigned int)snapshot.last_wifi_disconnect_reason,
        (int)snapshot.last_wifi_disconnect_rssi,
        (unsigned long long)snapshot.last_wifi_disconnect_uptime_ms,
        (unsigned long long)snapshot.last_wifi_reconnect_uptime_ms,
        snapshot.matter_server_ready ? "true" : "false",
        (unsigned long long)snapshot.matter_server_ready_uptime_ms,
        (unsigned long)snapshot.matter_application_command_callback_count,
        (unsigned long)snapshot.health_request_count,
        (unsigned long long)snapshot.last_health_request_uptime_ms,
        (unsigned long)snapshot.command_received_count,
        (unsigned long)snapshot.command_completed_count,
        (unsigned long)snapshot.command_busy_count,
        (unsigned long)snapshot.command_queue_overwrite_count,
        (unsigned int)DIAGNOSTICS_COMMAND_HISTORY_SIZE);

    if (length < 0 || (size_t)length >= sizeof(chunk)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "diagnostics serialization failed");
    }
    response_err = httpd_resp_send_chunk(req, chunk, length);
    if (response_err != ESP_OK) return response_err;

    for (uint8_t i = 0; i < snapshot.command_history_count; ++i) {
        const diagnostics_command_record_t &record = snapshot.commands[i];
        uint64_t handler_duration_ms = 0;
        if (record.completed_uptime_ms >= record.started_uptime_ms &&
            record.started_uptime_ms > 0) {
            handler_duration_ms = record.completed_uptime_ms - record.started_uptime_ms;
        }

        length = snprintf(
            chunk, sizeof(chunk),
            "%s{\"sequence\":%lu,\"type\":\"%s\",\"origin\":\"%s\","
            "\"target\":\"%s\",\"stage\":\"%s\",\"result\":\"%s\","
            "\"receivedUptimeMs\":%llu,\"queuedUptimeMs\":%llu,"
            "\"startedUptimeMs\":%llu,\"relayCommandedUptimeMs\":%llu,"
            "\"completedUptimeMs\":%llu,\"handlerDurationMs\":%llu}",
            i == 0 ? "" : ",",
            (unsigned long)record.sequence,
            diagnostics_command_type_name(record.type),
            diagnostics_command_origin_name(record.origin),
            record.target_unlock ? "unlocked" : "locked",
            diagnostics_command_stage_name(record.stage),
            diagnostics_command_result_name(record.result),
            (unsigned long long)record.received_uptime_ms,
            (unsigned long long)record.queued_uptime_ms,
            (unsigned long long)record.started_uptime_ms,
            (unsigned long long)record.relay_commanded_uptime_ms,
            (unsigned long long)record.completed_uptime_ms,
            (unsigned long long)handler_duration_ms);
        if (length < 0 || (size_t)length >= sizeof(chunk)) {
            httpd_resp_sendstr_chunk(req, nullptr);
            return ESP_FAIL;
        }
        response_err = httpd_resp_send_chunk(req, chunk, length);
        if (response_err != ESP_OK) return response_err;
    }

    response_err = httpd_resp_sendstr_chunk(req, "]}}");
    if (response_err != ESP_OK) return response_err;
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static void start_log_httpd(void)
{
    if (s_log_httpd) return;  // 이미 실행 중

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 6144;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_log_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "로그 HTTP 서버 시작 실패");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = log_handler_html,
    };
    static const httpd_uri_t uri_logs = {
        .uri     = "/logs",
        .method  = HTTP_GET,
        .handler = log_handler_json,
    };
    static const httpd_uri_t uri_diagnostics = {
        .uri     = "/diagnostics",
        .method  = HTTP_GET,
        .handler = diagnostics_handler_json,
    };
    static const httpd_uri_t uri_diag = {
        .uri     = "/diag",
        .method  = HTTP_GET,
        .handler = diagnostics_handler_json,
    };
    const httpd_uri_t *uris[] = { &uri_root, &uri_logs, &uri_diag, &uri_diagnostics };
    for (const httpd_uri_t *uri : uris) {
        esp_err_t register_err = httpd_register_uri_handler(s_log_httpd, uri);
        if (register_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP URI 등록 실패: uri=%s error=%s",
                     uri->uri, esp_err_to_name(register_err));
        }
    }

    /* 자신의 IP 주소 로그 출력 */
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "로그 서버: http://" IPSTR " (브라우저에서 확인)", IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI(TAG, "로그 서버 시작됨 (port 80)");
    }
}

static uint16_t s_door_endpoint_id = 1;

#define GITHUB_API_LATEST_URL       "https://api.github.com/repos/muinlab/esp-matter-deadbolt/releases/latest"
#define AUTO_OTA_BOOT_DELAY_MS      (15UL * 1000UL)          // 부팅 후 첫 확인 전 대기
#define AUTO_OTA_CHECK_INTERVAL_MS  (1UL * 3600UL * 1000UL)  // 1시간마다 재확인

static bool s_ota_pending = false;

/* GitHub API에서 최신 릴리즈 tag_name 추출 */
static bool fetch_latest_version(char *out, size_t max_len)
{
    esp_http_client_config_t cfg = {
        .url            = GITHUB_API_LATEST_URL,
        .timeout_ms     = 10000,
        .buffer_size    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "esp-matter-deadbolt");
    esp_http_client_set_header(client, "Accept",     "application/vnd.github+json");

    bool found = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buf[2048] = {};
        int  len = esp_http_client_read(client, buf, sizeof(buf) - 1);
        if (len > 0) {
            const char *key = "\"tag_name\":\"";
            char *p = strstr(buf, key);
            if (p) {
                p += strlen(key);
                size_t i = 0;
                while (*p && *p != '"' && i < max_len - 1) out[i++] = *p++;
                out[i] = '\0';
                found = (i > 0);
            }
        }
    }
    esp_http_client_cleanup(client);
    return found;
}

static void auto_ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(AUTO_OTA_BOOT_DELAY_MS));  // Matter 안정화 대기

    while (true) {
        if (!is_matter_connected()) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        char latest[32] = {};
        if (fetch_latest_version(latest, sizeof(latest))) {
            ESP_LOGI(TAG, "[AutoOTA] 현재=%s 최신=%s", FIRMWARE_VERSION, latest);
            if (strcmp(latest, FIRMWARE_VERSION) != 0) {
                ESP_LOGI(TAG, "[AutoOTA] 새 버전 감지 → 도어 잠김 대기");
                s_ota_pending = true;
            }
        } else {
            ESP_LOGW(TAG, "[AutoOTA] 버전 확인 실패 — 다음 주기에 재시도");
        }

        /* 새 버전이 있으면 도어 잠길 때까지 5초 간격으로 폴링 */
        if (s_ota_pending) {
            while (!door_is_locked()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            ESP_LOGI(TAG, "[AutoOTA] 도어 잠김 확인 → 자동 OTA 시작");
            s_ota_pending = false;
            xTaskCreate(ota_task, "ota_task", 8192, nullptr, 5, nullptr);
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(AUTO_OTA_CHECK_INTERVAL_MS));
    }
}

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "=== HTTPS OTA 시작: %s ===", OTA_FIRMWARE_URL);
    status_led_set_ota(true);  // 보라 느린 점멸 — 다운로드 완료 or 재부팅까지 유지

    esp_http_client_config_t http_cfg = {
        .url = OTA_FIRMWARE_URL,
        .timeout_ms = 60000,
        .max_redirection_count = 10,  // GitHub 리다이렉트 대응
        .buffer_size     = 4096,      // GitHub 응답 헤더가 커서 기본 512B 부족
        .buffer_size_tx  = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA 완료 → 재부팅");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 실패: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void health_check_task(void *arg)
{
    // 힙
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_kb   = heap_free / 1024;
    if (heap_kb > 255) heap_kb = 255;

    // 업타임
    int64_t uptime_min = esp_timer_get_time() / 1000000LL / 60;

    // WiFi RSSI
    wifi_ap_record_t ap_info = {};
    bool wifi_ok    = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    int8_t rssi     = wifi_ok ? ap_info.rssi : -128;
    uint8_t rssi_pk = (uint8_t)((int)rssi + 128);

    // 도어 상태
    bool locked = door_is_locked();

    // 결과 코드: 1=정상 2=힙경고(<20KB) 3=WiFi끊김
    uint8_t result = 1;
    if (heap_kb < 20) result = 2;
    if (!wifi_ok)     result = 3;

    // uint32 팩킹
    uint32_t health_val =
        (uint32_t)(result & 0x3)            |
        ((uint32_t)(locked ? 1 : 0) << 2)   |
        ((uint32_t)(wifi_ok ? 1 : 0) << 3)  |
        ((uint32_t)(heap_kb & 0xFF) << 8)   |
        ((uint32_t)rssi_pk << 16);

    ESP_LOGI(TAG, "[HEALTH] door=%s heap=%luKB uptime=%lldm wifi=%s rssi=%ddBm result=%d val=0x%08lX",
             locked ? "LOCKED" : "UNLOCKED",
             (unsigned long)heap_kb,
             (long long)uptime_min,
             wifi_ok ? "OK" : "DISCONNECTED",
             (int)rssi,
             result,
             (unsigned long)health_val);

    esp_matter_attr_val_t val = esp_matter_uint32(health_val);
    attribute::update(s_door_endpoint_id, CUSTOM_CLUSTER_CONTROL, CUSTOM_ATTR_HEALTH, &val);

    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  CHIP Door Lock Cluster Required Callbacks
 * ═══════════════════════════════════════════════════════════ */

void emberAfDoorLockClusterInitCallback(chip::EndpointId endpoint)
{
    ESP_LOGI(TAG, "Door Lock cluster init: endpoint=%d", endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter LockDoor command: endpoint=%d", endpointId);
    door_queue_command(false);
    return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter UnlockDoor command: endpoint=%d", endpointId);
    door_queue_command(true);
    return true;
}

bool emberAfPluginDoorLockGetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
    CredentialTypeEnum credentialType, EmberAfPluginDoorLockCredentialInfo & credential)
{
    return false;  // No credentials supported
}

bool emberAfPluginDoorLockSetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
    chip::FabricIndex creator, chip::FabricIndex modifier,
    DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
    const chip::ByteSpan & credentialData)
{
    return false;
}

bool emberAfPluginDoorLockGetUser(chip::EndpointId endpointId, uint16_t userIndex,
    EmberAfPluginDoorLockUserInfo & user)
{
    return false;
}

bool emberAfPluginDoorLockSetUser(chip::EndpointId endpointId, uint16_t userIndex,
    chip::FabricIndex creator, chip::FabricIndex modifier,
    const chip::CharSpan & userName, uint32_t uniqueId,
    UserStatusEnum userStatus, UserTypeEnum usertype,
    CredentialRuleEnum credentialRule,
    const CredentialStruct * credentials, size_t totalCredentials)
{
    return false;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
    uint16_t userIndex, EmberAfPluginDoorLockWeekDaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
    uint16_t userIndex, EmberAfPluginDoorLockYearDaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
    EmberAfPluginDoorLockHolidaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
    uint16_t userIndex, DlScheduleStatus status, DaysMaskMap daysMask,
    uint8_t startHour, uint8_t startMinute, uint8_t endHour, uint8_t endMinute)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
    uint16_t userIndex, DlScheduleStatus status,
    uint32_t localStartTime, uint32_t localEndTime)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
    DlScheduleStatus status, uint32_t localStartTime,
    uint32_t localEndTime, OperatingModeEnum operatingMode)
{
    return DlStatus::kFailure;
}

bool emberAfPluginDoorLockOnDoorUnlockWithTimeoutCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    uint16_t timeout,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter UnlockWithTimeout command: endpoint=%d, timeout=%ds", endpointId, timeout);
    uint8_t duration = (timeout > 255) ? 255 : (uint8_t)timeout;
    door_exit_open(duration);
    return true;
}

void emberAfPluginDoorLockOnAutoRelock(chip::EndpointId endpointId)
{
    ESP_LOGI(TAG, "Door Lock auto-relock: endpoint=%d", endpointId);
}

/* door_control_task 제거 — door_queue_command()가 워커 태스크를 통해 비동기 실행 */

/* ═══════════════════════════════════════════════════════════
 *  Matter Identification Callback
 * ═══════════════════════════════════════════════════════════ */

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type=%u, effect=%u, variant=%u",
             type, effect_id, effect_variant);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Attribute Update Callback
 * ═══════════════════════════════════════════════════════════ */

static esp_err_t app_attribute_update_cb(
    callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE) return ESP_OK;

    /* 커스텀 클러스터 처리 */
    if (cluster_id == CUSTOM_CLUSTER_CONTROL) {
        if (attribute_id == CUSTOM_ATTR_FACTORY_RESET && val->val.u16 == FACTORY_RESET_MAGIC) {
            ESP_LOGE(TAG, "=== 원격 팩토리 리셋 요청 (0xDEAD) ===");
            chip::Server::GetInstance().ScheduleFactoryReset();
        } else if (attribute_id == CUSTOM_ATTR_EXIT_OPEN && val->val.u8 > 0) {
            ESP_LOGI(TAG, "퇴실 요청: %d초", val->val.u8);
            door_exit_open(val->val.u8);
        } else if (attribute_id == CUSTOM_ATTR_OTA_TRIGGER && val->val.u8 == 1) {
            ESP_LOGI(TAG, "OTA 트리거 수신");
            xTaskCreate(ota_task, "ota_task", 8192, nullptr, 5, nullptr);
        } else if (attribute_id == CUSTOM_ATTR_HEALTH && val->val.u32 == 1) {
            diagnostics_note_health_request();
            xTaskCreate(health_check_task, "health_chk", 4096, nullptr, 3, nullptr);
        }
        return ESP_OK;
    }

    if (cluster_id != DoorLock::Id) return ESP_OK;

    if (attribute_id == DoorLock::Attributes::LockState::Id) {
        /* 릴레이 동작은 emberAf 콜백에서만 처리. 여기서 조작하면 피드백 루프 발생. */
        ESP_LOGI(TAG, "Matter LockState 속성 변경: %d (%s)",
                 val->val.u8,
                 val->val.u8 == 1 ? "Locked" :
                 val->val.u8 == 2 ? "Unlocked" : "NotFullyLocked");
    }

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Event Callback (WiFi/Fabric 연결 상태 추적)
 * ═══════════════════════════════════════════════════════════ */

static void refresh_commissioning_led(void)
{
    auto & server = chip::Server::GetInstance();
    bool has_fabric = (server.GetFabricTable().FabricCount() > 0);
    bool window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
    status_led_set_commissioning(!has_fabric);
    ESP_LOGI(TAG, "LED 커미셔닝 상태: fabric=%d window=%d", has_fabric, window_open);
}

static void wifi_diagnostics_event_handler(void *arg, esp_event_base_t event_base,
                                           int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_STA_DISCONNECTED) return;

    const wifi_event_sta_disconnected_t *event =
        static_cast<const wifi_event_sta_disconnected_t *>(event_data);
    diagnostics_note_wifi_disconnected(event ? event->reason : 0,
                                       event ? event->rssi : -128);
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Matter 커미셔닝 완료");
            comm_set_matter_connected(true);
            refresh_commissioning_led();
            break;

        case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
            if (event->InterfaceIpAddressChanged.Type ==
                chip::DeviceLayer::InterfaceIpChangeType::kIpV4_Assigned) {
                diagnostics_note_wifi_connected();
                {
                    esp_netif_ip_info_t ip_info = {};
                    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&ip_info.ip));
                    }
                }
                ESP_LOGI(TAG, "WiFi IP 할당됨 → Matter 연결");
                comm_set_matter_connected(true);
                start_log_httpd();
                // WiFi 연결 후 Fabric이 있으면 60초간 커미셔닝 윈도우 오픈 (추가 기기 등록)
                if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
                    ESP_LOGI(TAG, "기존 Fabric 감지 → 커미셔닝 윈도우 180초 오픈 (추가 기기 등록 가능)");
                    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
                        .OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(180));
                    if (err != CHIP_NO_ERROR) {
                        ESP_LOGE(TAG, "커미셔닝 윈도우 오픈 실패: %" CHIP_ERROR_FORMAT, err.Format());
                    }
                }
            }
            break;

        case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
            if (event->WiFiConnectivityChange.Result ==
                chip::DeviceLayer::ConnectivityChange::kConnectivity_Lost) {
                /* 원시 WiFi 콜백과 중복되어도 diagnostics가 한 단절 구간으로 합친다. */
                diagnostics_note_wifi_disconnected(0, -128);
                ESP_LOGW(TAG, "WiFi 연결 끊김 → Matter 비활성");
                comm_set_matter_connected(false);
            }
            break;

        case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
            refresh_commissioning_led();
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                ESP_LOGI(TAG, "모든 Fabric 제거됨 → 커미셔닝 윈도우 오픈 (600초)");
                comm_set_matter_connected(false);
                chip::Server::GetInstance().GetCommissioningWindowManager()
                    .OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(600));
            }
            break;

        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
            refresh_commissioning_led();
            break;

        case chip::DeviceLayer::DeviceEventType::kServerReady:
            diagnostics_note_matter_server_ready();
            refresh_commissioning_led();
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Door Lock Endpoint 설정
 * ═══════════════════════════════════════════════════════════ */

static void add_custom_control_cluster(endpoint_t *ep)
{
    cluster_t *cluster = cluster::create(ep, CUSTOM_CLUSTER_CONTROL,
                                          CLUSTER_FLAG_SERVER);
    if (!cluster) {
        ESP_LOGE(TAG, "커스텀 클러스터 생성 실패");
        return;
    }

    /* Factory Reset: uint16, write 0xDEAD → 팩토리 리셋 */
    esp_matter_attr_val_t reset_val = esp_matter_uint16(0);
    attribute::create(cluster, CUSTOM_ATTR_FACTORY_RESET,
                      ATTRIBUTE_FLAG_WRITABLE, reset_val);

    /* Exit Open: uint8, write duration(3~30) → 퇴실 열림 후 자동 잠금 */
    esp_matter_attr_val_t exit_val = esp_matter_uint8(0);
    attribute::create(cluster, CUSTOM_ATTR_EXIT_OPEN,
                      ATTRIBUTE_FLAG_WRITABLE, exit_val);

    /* OTA Trigger: uint8, write 1 → HTTPS OTA 시작 */
    esp_matter_attr_val_t trigger_val = esp_matter_uint8(0);
    attribute::create(cluster, CUSTOM_ATTR_OTA_TRIGGER,
                      ATTRIBUTE_FLAG_WRITABLE, trigger_val);

    /* Health Check: uint32, write 1 → 측정 후 결과 저장, read → 마지막 결과 반환 */
    esp_matter_attr_val_t health_val = esp_matter_uint32(0);
    attribute::create(cluster, CUSTOM_ATTR_HEALTH,
                      ATTRIBUTE_FLAG_WRITABLE, health_val);

    ESP_LOGI(TAG, "커스텀 클러스터 등록 (0x%08lX): factory_reset(0), exit_open(1), ota_trigger(2), health(3)",
             (unsigned long)CUSTOM_CLUSTER_CONTROL);
}

static void setup_matter_door_lock(node_t *node)
{
    endpoint::door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = door_is_locked() ? 1 : 2;
    lock_config.door_lock.lock_type = 0;          // DeadBolt
    lock_config.door_lock.actuator_enabled = true;

    endpoint_t *ep = endpoint::door_lock::create(node, &lock_config,
                                                  ENDPOINT_FLAG_NONE, NULL);
    uint16_t endpoint_id = endpoint::get_id(ep);
    s_door_endpoint_id = endpoint_id;
    comm_set_endpoint_id(endpoint_id);


    /* 커스텀 제어 클러스터 추가 (factory_reset + exit_open) */
    add_custom_control_cluster(ep);

    ESP_LOGI(TAG, "Door Lock endpoint 생성: ID=%d", endpoint_id);
}

/* ═══════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════
 *  BOOT 버튼 팩토리 리셋 (GPIO 0, 5초 길게 누르기)
 * ═══════════════════════════════════════════════════════════ */

#define BOOT_BUTTON_GPIO      0
#define FACTORY_RESET_HOLD_MS 5000
#define BOOT_POLL_MS          100

static void factory_reset_task(void *arg)
{
    // BOOT 버튼 (GPIO 0): LOW = 눌림, HIGH = 안 눌림
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    uint32_t hold_ms = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));

        if (gpio_get_level((gpio_num_t)BOOT_BUTTON_GPIO) == 0) {
            // 버튼 눌림
            hold_ms += BOOT_POLL_MS;

            if (hold_ms == 2000) {
                ESP_LOGW(TAG, "BOOT 버튼 2초... 계속 누르면 팩토리 리셋");
                status_led_notify_op_fail();  // 빨강 깜빡임 경고
            }

            if (hold_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGE(TAG, "=== 팩토리 리셋 실행! ===");

                // 1. Matter fabric 제거
                chip::Server::GetInstance().ScheduleFactoryReset();

                // ScheduleFactoryReset이 NVS 삭제 + 재부팅 처리
                // 여기까지 오면 재부팅 대기
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        } else {
            // 버튼 놓음
            if (hold_ms > 0 && hold_ms < FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "BOOT 버튼 놓음 (%"PRIu32"ms, 리셋 취소)", hold_ms);
            }
            hold_ms = 0;
        }
    }
}

extern "C" void app_main(void)
{
    // 0. 최우선: 릴레이 GPIO 즉시 LOW (잠금 상태)
    gpio_reset_pin((gpio_num_t)GPIO_RELAY_PIN);
    gpio_set_direction((gpio_num_t)GPIO_RELAY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)GPIO_RELAY_PIN, 0);  // LOW = 잠금

    // 0-1. GPIO 전체 초기화
    gpio_init();

    // 안전 GPIO 설정 이후에만 로그/진단 초기화
    esp_log_set_vprintf(deadbolt_vprintf);
    diagnostics_init(esp_reset_reason());
    ESP_LOGI(TAG, "=== ESP32-S3 Matter Door Lock 시작 ===");

    // 1. NVS 초기화
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 파티션 초기화 필요 → erase & re-init");
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. (BLE는 Matter 커미셔닝 전용 — CHIP SDK가 관리)

    // 3. Matter 노드 생성
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == NULL) {
        ESP_LOGE(TAG, "Matter 노드 생성 실패!");
        return;
    }

    // 4. Door Lock 엔드포인트 생성
    setup_matter_door_lock(node);

    // 5. 도어 컨트롤러 초기화 (폴링/타이머)
    door_controller_init();

    // 6. Matter 시작 (NimBLE 초기화 포함)
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter 시작 실패: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     wifi_diagnostics_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 진단 이벤트 등록 실패: %s", esp_err_to_name(err));
    }

    // 7. (BLE GATT 제거 — Matter 커미셔닝 전용)

    // 8. Status LED 초기화 + 시스템 준비 완료
    status_led_init();
    status_led_set_locked(door_is_locked());
    status_led_set_commissioning(true);  // 초기: 커미셔닝 대기 (파랑 빠른 깜빡임)
    status_led_set_system_ready(true);

    // 9. BOOT 버튼 팩토리 리셋 태스크
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 3, NULL);

    // 10. 자동 OTA 버전 체크 태스크 (60초 후 시작, 6시간 주기)
    xTaskCreate(auto_ota_task, "auto_ota", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== Matter Door Lock 시작 완료 ===");
    ESP_LOGI(TAG, "  Primary:  WiFi over Matter (Door Lock Cluster)");
    ESP_LOGI(TAG, "  Fallback: BLE GATT Server (AES-128-GCM)");
    ESP_LOGI(TAG, "  릴레이:   GPIO%d (CW-020, LOW=잠금 HIGH=해제)", GPIO_RELAY_PIN);
}
