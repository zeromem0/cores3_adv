/*
 * Logging macros mapped onto ESP-IDF logging.
 *
 * The application's usual host keeps a persisted log ring buffer
 * behind these macros;
 * this firmware has no equivalent, so they forward to esp_log.
 */
#pragma once

#include "esp_log.h"

#define DHEX_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define DHEX_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define DHEX_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define DHEX_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
