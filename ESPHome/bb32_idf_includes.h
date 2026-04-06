#pragma once
// ESP-IDF headers needed by bb32.yaml lambdas that aren't pulled in
// automatically by the ESPHome build context.
#include "esp_timer.h"      // esp_timer_get_time()
#include "esp_wifi.h"       // esp_wifi_sta_get_ap_info(), wifi_ap_record_t
#include "esp_system.h"     // esp_get_free_heap_size(), esp_reset_reason()
