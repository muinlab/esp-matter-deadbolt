#pragma once

static inline void diagnostics_test_log(const char *, const char *, ...) {}

#define ESP_LOGI(...) diagnostics_test_log(__VA_ARGS__)
#define ESP_LOGW(...) diagnostics_test_log(__VA_ARGS__)
#define ESP_LOGE(...) diagnostics_test_log(__VA_ARGS__)
