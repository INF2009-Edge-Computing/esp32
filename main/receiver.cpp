extern "C" {
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include <netdb.h>
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "rom/ets_sys.h"
#include "cJSON.h"
#include "esp_crc.h"
}

#include "tflm_inference.h"

// Node / network identity — configure via 'idf.py menuconfig' > CSI Node Configuration
#define ESP32_ID_STR    CONFIG_CSI_NODE_ID
#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASS       CONFIG_WIFI_PASS
#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI
#define HTTP_SERVER_BASE CONFIG_HTTP_SERVER_BASE
#define HTTP_UPLOAD_URI  CONFIG_HTTP_SERVER_BASE "/upload_data"
#define HTTP_MODEL_URI   CONFIG_HTTP_SERVER_BASE "/model/" CONFIG_CSI_NODE_ID
#define HTTP_PARAMS_URI  CONFIG_HTTP_SERVER_BASE "/params/" CONFIG_CSI_NODE_ID

// MQTT topic construction uses the node ID string from Kconfig
#define ESP32_COLLECT_TOPIC           "/commands/" CONFIG_CSI_NODE_ID "/collect"
#define ESP32_DOWNLOAD_TOPIC          "/commands/" CONFIG_CSI_NODE_ID "/update_model"
#define ESP32_TRAINING_COMPLETE_TOPIC "/commands/" CONFIG_CSI_NODE_ID "/training_complete"
#define ESP32_LOAD_MODEL_TOPIC        "/commands/" CONFIG_CSI_NODE_ID "/load_model"
#define ESP32_STATUS_TOPIC            "/sensors/"  CONFIG_CSI_NODE_ID "/status"
#define ESP32_PERF_BIN_TOPIC          "/sensors/"  CONFIG_CSI_NODE_ID "/perf_bin"
#define ESP32_DEVICE_STATUS_TOPIC     "device/" CONFIG_CSI_NODE_ID "/status"
#define DEFAULT_SCAN_LIST_SIZE 20

/* Define range of subcarriers to use for CSI, ignoring noisy/unused ones */
#define MAX_LOWER 4
#define MAX_UPPER 60
#define DC_NULL 32
#define NUM_SUBCARRIERS 64
#define EXPECTED_PERIOD_US 100000 // 10Hz expected period between packets

#define ACTIVE_SUBCARRIERS (MAX_UPPER - MAX_LOWER)
#define COMPACT_FEATURE_COUNT 51
#define LEGACY_RAW55_FEATURE_COUNT 55
#define LEGACY_RAW56_FEATURE_COUNT ACTIVE_SUBCARRIERS
#define MAX_MODEL_FEATURES_PER_FRAME (ACTIVE_SUBCARRIERS + 8)
#define NOTEBOOK_MAX_EXTRACT_WINDOW 16
#define FEATURE_EPSILON 1e-6f
static int64_t last_trigger_us = 0; // Last time we accepted a packet for processing

#define SAMPLE_SIZE 200                                  // 20 seconds of data at 10Hz
#define SUB_BATCH_SIZE 40                                // packets per HTTP sub-batch
static uint8_t csi_buffer[SUB_BATCH_SIZE][ACTIVE_SUBCARRIERS];

// Idempotency key format: "<session_id>_<sub_batch_idx>".
// session_id is typically a timestamp string (~14 chars), and the index is small.
// Reserve a conservative upper bound (including null terminator).
static const size_t IDEMPOTENCY_KEY_MAX_LEN = 64;
static uint8_t packet_idx = 0;
static uint8_t sub_batch_idx = 0;
static bool is_collecting = false;
static char collection_label[32] = "unknown";
static char current_session_id[32] = "";
static uint8_t dynamic_target_mac[6] = {0};

static const char *TAG = "logger";
static uint8_t *model_buffer = NULL;
static size_t model_size = 0;
static bool model_ready = false;
static bool model_download_armed = false;
static bool auto_load_model_on_reconnect = true;  // Auto-reload model on MQTT connection
static float mean_vals[MAX_MODEL_FEATURES_PER_FRAME] = {0};
static float std_vals[MAX_MODEL_FEATURES_PER_FRAME] = {0};
static float *model_window_ring = NULL;
static float *model_input_scratch = NULL;
static size_t model_input_elements = ACTIVE_SUBCARRIERS;
static size_t model_features_per_frame = ACTIVE_SUBCARRIERS;
static size_t model_window_size = 1;
static size_t model_window_fill = 0;
static size_t model_window_head = 0;
static bool notebook_mode_enabled = false;
static bool grouped_mode_enabled = false;
static size_t grouped_feature_count = 0;
static int selected_subcarriers[MAX_MODEL_FEATURES_PER_FRAME] = {0};
static int selected_raw_indices[MAX_MODEL_FEATURES_PER_FRAME] = {0};
static size_t selected_subcarrier_count = 0;
static int notebook_extract_window_size = 5;
static int notebook_calibration_frames = 20;
static bool notebook_use_session_offset = true;
static float notebook_train_baseline = 1.0f;
static float notebook_train_mean = 0.0f;
static float notebook_feature_ring[NOTEBOOK_MAX_EXTRACT_WINDOW][MAX_MODEL_FEATURES_PER_FRAME] = {{0}};
static size_t notebook_ring_count = 0;
static size_t notebook_ring_head = 0;
static float notebook_session_mean_accum = 0.0f;
static uint32_t notebook_session_mean_count = 0;
static float notebook_variation_baseline_accum = 0.0f;
static uint32_t notebook_variation_baseline_count = 0;
static float last_prediction_confidence = 0.0f;
static float last_prediction_probs[3] = {0.3333f, 0.3333f, 0.3333f};
static float last_csi_mean = 0.0f;
static float last_csi_max = 0.0f;
static float smoothed_probs[3] = {0.3333f, 0.3333f, 0.3333f};
static int stable_state = -1;
static int pending_state = -1;
static int pending_count = 0;
static float confidence_ema = 0.0f;
static float entropy_ema = 0.0f;
static float feature_abs_ema = 0.0f;
static int drift_alert_streak = 0;
static int64_t last_drift_report_us = 0;
static int64_t last_uncertain_publish_us = 0;
static int64_t last_hold_publish_us = 0;
static bool wifi_connected = false;
static bool mqtt_connected = false;
static bool model_download_in_progress = false;

// Status heartbeat task: send only when key state changes (or periodically)
// to reduce heap allocations / wakeups.
#define HEARTBEAT_INTERVAL_MS (60 * 1000) // 1 minute
#define INFERENCE_QUEUE_DEPTH 16
#define INFERENCE_DROP_LOG_INTERVAL 100
#define CSI_LOG_MIN_INTERVAL_MS 1000
#define STATE_SMOOTHING_ALPHA 0.85f
#define STATE_ENTER_THRESHOLD 0.72f
#define STATE_EXIT_THRESHOLD 0.55f
#define STATE_SWITCH_CONSECUTIVE 3
#define UNCERTAIN_CONFIDENCE_THRESHOLD 0.45f
#define UNCERTAIN_PUBLISH_MIN_INTERVAL_MS 5000
#define HOLD_STATE_PUBLISH_MIN_INTERVAL_MS 3000
#define DRIFT_REPORT_INTERVAL_MS (30 * 1000)
#define DRIFT_ALERT_CONSECUTIVE 3
#define DRIFT_ENTROPY_THRESHOLD 0.72f
#define DRIFT_CONFIDENCE_THRESHOLD 0.42f
#define DRIFT_FEATURE_ABS_THRESHOLD 2.2f
#define PASO_INFERENCE_BUDGET_US 50000
#define PASO_PIPELINE_BUDGET_US 500000
#define PASO_PERF_PUBLISH_INTERVAL_SAMPLES 50

static TaskHandle_t status_task_handle = NULL;
static bool last_published_mqtt_connected = false;
static bool last_published_model_ready = false;
static bool last_published_model_download_armed = false;
static int64_t last_csi_log_us = 0;

static void notify_status_task(void)
{
    if (status_task_handle != NULL) {
        xTaskNotifyGive(status_task_handle);
    }
}

typedef struct {
    uint8_t normalized[ACTIVE_SUBCARRIERS];
} inference_sample_t;

static QueueHandle_t inference_queue = NULL;
static uint32_t inference_drop_count = 0;

typedef struct {
    char *label;
    uint8_t sub_batch_idx;
    uint8_t total_sub_batches;
    char session_id[32];
    uint8_t *payload;
    size_t payload_len;
    uint32_t payload_crc32;
} http_task_params_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t reserved;
    uint16_t payload_size;
    uint32_t invoke_last_us;
    uint32_t invoke_avg_us;
    uint32_t invoke_min_us;
    uint32_t invoke_max_us;
    uint32_t sample_count;
    int32_t queue_wait_us;
    int32_t feature_us;
    int32_t invoke_stage_us;
    int32_t pipeline_total_us;
    uint32_t free_heap;
} paso_perf_payload_t;

static esp_http_client_handle_t client = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;

// Cap the exponent to prevent excessive delays (base_ms * 2^exponent).
// With base_ms=500 and max_exponent=6, the maximum base delay is 32s.
static const int MAX_BACKOFF_EXPONENT = 6;

static uint32_t backoff_jitter_ms(int attempt, uint32_t base_ms) {
    // exponential backoff with +/-50% jitter: base * 2^exponent +/- 50%
    // Range: [cap/2, 3*cap/2]
    int backoff_exponent = (attempt < MAX_BACKOFF_EXPONENT) ? attempt : MAX_BACKOFF_EXPONENT;
    uint32_t cap = base_ms * (1u << backoff_exponent);
    uint32_t half = cap / 2;
    // jitter in [0, cap]
    uint32_t jitter = esp_random() % (cap + 1);
    uint32_t val = cap - half + jitter; // [cap/2, 3*cap/2]
    return val < base_ms ? base_ms : val;
}

static bool publish_with_retry_ex(const char *topic, const char *payload, int qos, bool retain)
{
    if (mqtt_client == NULL) {
        ESP_LOGW(TAG, "publish_with_retry: mqtt_client is NULL (topic=%s)", topic);
        return false;
    }

    const int max_attempts = 5;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain ? 1 : 0);
        if (msg_id >= 0) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "publish succeeded after %d retries (topic=%s)", attempt, topic);
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt)));
    }

    ESP_LOGW(TAG, "publish failed after %d attempts (topic=%s payload=%s)", max_attempts, topic, payload);
    return false;
}

static bool publish_with_retry(const char *topic, const char *payload, int qos)
{
    return publish_with_retry_ex(topic, payload, qos, false);
}

static void publish_status(const char *event, bool include_model_status, bool retain)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", event);
    if (include_model_status) {
        cJSON_AddBoolToObject(obj, "model_ready", model_ready);
        cJSON_AddBoolToObject(obj, "model_download_armed", model_download_armed);
    }
    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry_ex(ESP32_STATUS_TOPIC, json_str, 1, retain);
        free(json_str);
    }
    cJSON_Delete(obj);
}

static void publish_ack(const char *cmd)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", "ack");
    cJSON_AddStringToObject(obj, "cmd", cmd ? cmd : "unknown");

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);
}

static int32_t clamp_i64_to_i32(int64_t value)
{
    if (value > 2147483647LL) {
        return 2147483647;
    }
    if (value < -2147483648LL) {
        return -2147483648;
    }
    return (int32_t)value;
}

static void publish_perf_binary_metrics(int64_t queue_wait_us,
                                        int64_t feature_us,
                                        int64_t invoke_stage_us,
                                        int64_t pipeline_total_us)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        return;
    }

    tflm_inference_profile_t prof = {};
    if (!tflm_get_inference_profile(&prof)) {
        return;
    }

    paso_perf_payload_t payload = {};
    payload.version = 1;
    payload.reserved = 0;
    payload.payload_size = (uint16_t)sizeof(payload);
    payload.invoke_last_us = prof.last_us;
    payload.invoke_avg_us = prof.avg_us;
    payload.invoke_min_us = prof.min_us;
    payload.invoke_max_us = prof.max_us;
    payload.sample_count = prof.sample_count;
    payload.queue_wait_us = clamp_i64_to_i32(queue_wait_us);
    payload.feature_us = clamp_i64_to_i32(feature_us);
    payload.invoke_stage_us = clamp_i64_to_i32(invoke_stage_us);
    payload.pipeline_total_us = clamp_i64_to_i32(pipeline_total_us);
    payload.free_heap = esp_get_free_heap_size();

    int msg_id = esp_mqtt_client_publish(mqtt_client,
                                         ESP32_PERF_BIN_TOPIC,
                                         (const char *)&payload,
                                         (int)sizeof(payload),
                                         0,
                                         0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish perf binary payload topic=%s", ESP32_PERF_BIN_TOPIC);
    }
}

static void publish_model_download_memory_error(const char *asset,
                                                const char *reason,
                                                const char *url,
                                                size_t requested_bytes,
                                                int attempt,
                                                esp_err_t err)
{
    const char *asset_safe = asset ? asset : "unknown";
    const char *reason_safe = reason ? reason : "unspecified";
    const char *url_safe = url ? url : "";
    const char *err_name = esp_err_to_name(err);
    if (err_name == NULL) {
        err_name = "UNKNOWN";
    }

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    char payload[512];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"event\":\"model_download_memory_error\","
                       "\"asset\":\"%s\","
                       "\"reason\":\"%s\","
                       "\"attempt\":%d,"
                       "\"requested_bytes\":%u,"
                       "\"free_heap\":%" PRIu32 ","
                       "\"largest_block\":%" PRIu32 ","
                       "\"err\":%d,"
                       "\"err_name\":\"%s\","
                       "\"url\":\"%s\"}",
                       asset_safe,
                       reason_safe,
                       attempt,
                       (unsigned)requested_bytes,
                       free_heap,
                       largest_block,
                       (int)err,
                       err_name,
                       url_safe);

    ESP_LOGW(TAG,
             "Model download memory error asset=%s reason=%s attempt=%d req=%u free=%" PRIu32 " largest=%" PRIu32 " err=%s",
             asset_safe,
             reason_safe,
             attempt,
             (unsigned)requested_bytes,
             free_heap,
             largest_block,
             err_name);

    if (len > 0 && len < (int)sizeof(payload)) {
        publish_with_retry(ESP32_STATUS_TOPIC, payload, 1);
    }
}

static void publish_model_download_incompatible(const char *reason, size_t input_elements)
{
    const char *reason_safe = reason ? reason : "unspecified";
    char payload[256];
    int len = snprintf(payload,
                       sizeof(payload),
                       "{\"event\":\"model_download_incompatible\","
                       "\"reason\":\"%s\","
                       "\"input_elements\":%u,"
                       "\"feature_count\":%d}",
                       reason_safe,
                       (unsigned)input_elements,
                       (int)model_features_per_frame);
    if (len > 0 && len < (int)sizeof(payload)) {
        publish_with_retry(ESP32_STATUS_TOPIC, payload, 1);
    }
}

static void free_model_input_buffers(void)
{
    if (model_window_ring != NULL) {
        free(model_window_ring);
        model_window_ring = NULL;
    }
    if (model_input_scratch != NULL) {
        free(model_input_scratch);
        model_input_scratch = NULL;
    }
    model_input_elements = model_features_per_frame;
    model_window_size = 1;
    model_window_fill = 0;
    model_window_head = 0;
    notebook_ring_count = 0;
    notebook_ring_head = 0;
    notebook_session_mean_accum = 0.0f;
    notebook_session_mean_count = 0;
    notebook_variation_baseline_accum = 0.0f;
    notebook_variation_baseline_count = 0;
}

static int raw_index_for_subcarrier(int sc)
{
    if (sc < MAX_LOWER || sc > MAX_UPPER || sc == DC_NULL) {
        return -1;
    }
    if (sc < DC_NULL) {
        return sc - MAX_LOWER;
    }
    return sc - MAX_LOWER - 1;
}

static bool is_supported_feature_layout(size_t feature_count)
{
    if (grouped_mode_enabled) {
        return grouped_feature_count > 0 && feature_count == (grouped_feature_count + 1);
    }

    if (notebook_mode_enabled) {
        return selected_subcarrier_count > 0 && feature_count == (selected_subcarrier_count + 1);
    }

    return feature_count == COMPACT_FEATURE_COUNT ||
           feature_count == LEGACY_RAW55_FEATURE_COUNT ||
           feature_count == LEGACY_RAW56_FEATURE_COUNT;
}

static float compute_subcarrier_slope(const uint8_t raw_row[ACTIVE_SUBCARRIERS],
                                      int sc_start,
                                      int sc_end);

static float notebook_avg_variation_from_ring(void)
{
    if (selected_subcarrier_count == 0 || notebook_ring_count == 0) {
        return 0.0f;
    }

    size_t window_n = notebook_ring_count;
    if (window_n > (size_t)notebook_extract_window_size) {
        window_n = (size_t)notebook_extract_window_size;
    }
    if (window_n == 0) {
        return 0.0f;
    }

    size_t start = (notebook_ring_head + NOTEBOOK_MAX_EXTRACT_WINDOW - window_n) % NOTEBOOK_MAX_EXTRACT_WINDOW;
    float std_accum = 0.0f;

    for (size_t feat = 0; feat < selected_subcarrier_count; feat++) {
        float mean = 0.0f;
        for (size_t w = 0; w < window_n; w++) {
            size_t idx = (start + w) % NOTEBOOK_MAX_EXTRACT_WINDOW;
            mean += notebook_feature_ring[idx][feat];
        }
        mean /= (float)window_n;

        float var = 0.0f;
        for (size_t w = 0; w < window_n; w++) {
            size_t idx = (start + w) % NOTEBOOK_MAX_EXTRACT_WINDOW;
            float d = notebook_feature_ring[idx][feat] - mean;
            var += d * d;
        }
        var /= (float)window_n;
        std_accum += sqrtf(var);
    }

    return std_accum / (float)selected_subcarrier_count;
}

static bool build_notebook_frame_features(const uint8_t raw_row[ACTIVE_SUBCARRIERS],
                                          float *out_features,
                                          size_t out_count)
{
    if (!notebook_mode_enabled ||
        selected_subcarrier_count == 0 ||
        out_count != (selected_subcarrier_count + 1) ||
        out_features == NULL ||
        raw_row == NULL) {
        return false;
    }

    float frame_mean = 0.0f;
    for (size_t i = 0; i < selected_subcarrier_count; i++) {
        int raw_idx = selected_raw_indices[i];
        if (raw_idx < 0 || raw_idx >= ACTIVE_SUBCARRIERS) {
            return false;
        }

        float v = (float)raw_row[raw_idx];
        out_features[i] = v;
        frame_mean += v;
        notebook_feature_ring[notebook_ring_head][i] = v;
    }

    notebook_ring_head = (notebook_ring_head + 1) % NOTEBOOK_MAX_EXTRACT_WINDOW;
    if (notebook_ring_count < NOTEBOOK_MAX_EXTRACT_WINDOW) {
        notebook_ring_count++;
    }

    frame_mean /= (float)selected_subcarrier_count;
    notebook_session_mean_accum += frame_mean;
    notebook_session_mean_count++;

    float session_mean = notebook_train_mean;
    if (notebook_session_mean_count > 0) {
        session_mean = notebook_session_mean_accum / (float)notebook_session_mean_count;
    }
    float session_offset = notebook_use_session_offset ? (notebook_train_mean - session_mean) : 0.0f;

    for (size_t i = 0; i < selected_subcarrier_count; i++) {
        out_features[i] += session_offset;
    }

    float avg_variation = notebook_avg_variation_from_ring();
    if (notebook_variation_baseline_count < (uint32_t)notebook_calibration_frames) {
        notebook_variation_baseline_accum += avg_variation;
        notebook_variation_baseline_count++;
    }

    float session_baseline = notebook_train_baseline;
    if (notebook_variation_baseline_count > 0) {
        session_baseline = notebook_variation_baseline_accum / (float)notebook_variation_baseline_count;
    }
    if (session_baseline <= FEATURE_EPSILON) {
        session_baseline = 1.0f;
    }

    out_features[selected_subcarrier_count] = avg_variation / (session_baseline + FEATURE_EPSILON);
    return true;
}

static bool build_grouped_frame_features(const uint8_t raw_row[ACTIVE_SUBCARRIERS],
                                         float *out_features,
                                         size_t out_count)
{
    if (!grouped_mode_enabled ||
        grouped_feature_count == 0 ||
        out_features == NULL ||
        raw_row == NULL ||
        out_count != (grouped_feature_count + 1)) {
        return false;
    }

    size_t base_group_len = ACTIVE_SUBCARRIERS / grouped_feature_count;
    size_t remainder = ACTIVE_SUBCARRIERS % grouped_feature_count;
    size_t cursor = 0;

    for (size_t g = 0; g < grouped_feature_count; g++) {
        size_t this_group_len = base_group_len + ((g < remainder) ? 1 : 0);
        if (this_group_len == 0 || (cursor + this_group_len) > ACTIVE_SUBCARRIERS) {
            return false;
        }

        float sum = 0.0f;
        for (size_t k = 0; k < this_group_len; k++) {
            sum += (float)raw_row[cursor + k];
        }
        out_features[g] = sum / (float)this_group_len;
        cursor += this_group_len;
    }

    float mean = 0.0f;
    for (size_t g = 0; g < grouped_feature_count; g++) {
        mean += out_features[g];
    }
    mean /= (float)grouped_feature_count;

    float var = 0.0f;
    for (size_t g = 0; g < grouped_feature_count; g++) {
        float d = out_features[g] - mean;
        var += d * d;
    }
    var /= (float)grouped_feature_count;
    out_features[grouped_feature_count] = sqrtf(var);
    return true;
}

static bool build_legacy_frame_features(const uint8_t raw_row[ACTIVE_SUBCARRIERS],
                                        float *out_features,
                                        size_t out_count)
{
    if (raw_row == NULL || out_features == NULL || out_count == 0 || out_count > MAX_MODEL_FEATURES_PER_FRAME) {
        return false;
    }

    // Legacy layout: SC_4..60 excluding SC_32 => 56 features.
    if (out_count == LEGACY_RAW56_FEATURE_COUNT) {
        for (size_t i = 0; i < ACTIVE_SUBCARRIERS; i++) {
            out_features[i] = (float)raw_row[i];
        }
        return true;
    }

    // Legacy layout: SC_4..59 excluding SC_32 => 55 features.
    if (out_count == LEGACY_RAW55_FEATURE_COUNT) {
        size_t out_idx = 0;
        for (int sc = 4; sc <= 59; sc++) {
            if (sc == DC_NULL) {
                continue;
            }
            int idx = raw_index_for_subcarrier(sc);
            if (idx < 0 || idx >= ACTIVE_SUBCARRIERS || out_idx >= out_count) {
                return false;
            }
            out_features[out_idx++] = (float)raw_row[idx];
        }
        return out_idx == out_count;
    }

    // Compact layout must match training profile exactly.
    if (out_count != COMPACT_FEATURE_COUNT) {
        return false;
    }

    float compact[COMPACT_FEATURE_COUNT] = {0};
    size_t compact_idx = 0;

    // Keep SC_10..58 (excluding DC null), deprioritize SC_4..9 and SC_59..60.
    for (int sc = 10; sc <= 58; sc++) {
        if (sc == DC_NULL) {
            continue;
        }
        int idx = raw_index_for_subcarrier(sc);
        if (idx < 0 || idx >= ACTIVE_SUBCARRIERS) {
            return false;
        }
        if (compact_idx >= COMPACT_FEATURE_COUNT) {
            return false;
        }
        compact[compact_idx++] = (float)raw_row[idx];
    }

    float low_sum = 0.0f;
    float high_sum = 0.0f;
    int low_n = 0;
    int high_n = 0;
    for (int sc = 4; sc <= 26; sc++) {
        int idx = raw_index_for_subcarrier(sc);
        if (idx >= 0 && idx < ACTIVE_SUBCARRIERS) {
            low_sum += (float)raw_row[idx];
            low_n++;
        }
    }
    for (int sc = 38; sc <= 60; sc++) {
        int idx = raw_index_for_subcarrier(sc);
        if (idx >= 0 && idx < ACTIVE_SUBCARRIERS) {
            high_sum += (float)raw_row[idx];
            high_n++;
        }
    }

    float low_mean = (low_n > 0) ? (low_sum / (float)low_n) : 0.0f;
    float high_mean = (high_n > 0) ? (high_sum / (float)high_n) : 0.0f;
    float band_ratio = low_mean / fmaxf(high_mean, FEATURE_EPSILON);
    float low_slope = compute_subcarrier_slope(raw_row, 10, 26);
    float high_slope = compute_subcarrier_slope(raw_row, 38, 55);

    if (compact_idx + 3 > COMPACT_FEATURE_COUNT) {
        return false;
    }
    compact[compact_idx++] = band_ratio;
    compact[compact_idx++] = low_slope;
    compact[compact_idx++] = high_slope;

    if (compact_idx != COMPACT_FEATURE_COUNT) {
        return false;
    }

    memcpy(out_features, compact, COMPACT_FEATURE_COUNT * sizeof(float));
    return true;
}

static float compute_subcarrier_slope(const uint8_t raw_row[ACTIVE_SUBCARRIERS], int sc_start, int sc_end)
{
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_xx = 0.0f;
    float sum_xy = 0.0f;
    int n = 0;

    for (int sc = sc_start; sc <= sc_end; sc++) {
        if (sc == DC_NULL) {
            continue;
        }
        int idx = raw_index_for_subcarrier(sc);
        if (idx < 0 || idx >= ACTIVE_SUBCARRIERS) {
            continue;
        }

        float x = (float)sc;
        float y = (float)raw_row[idx];
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        n++;
    }

    if (n < 2) {
        return 0.0f;
    }

    float denom = ((float)n * sum_xx) - (sum_x * sum_x);
    if (fabsf(denom) <= FEATURE_EPSILON) {
        return 0.0f;
    }

    return ((((float)n) * sum_xy) - (sum_x * sum_y)) / denom;
}

static bool build_frame_features(const uint8_t raw_row[ACTIVE_SUBCARRIERS],
                                 float *out_features,
                                 size_t out_count)
{
    if (grouped_mode_enabled) {
        return build_grouped_frame_features(raw_row, out_features, out_count);
    }
    if (notebook_mode_enabled) {
        return build_notebook_frame_features(raw_row, out_features, out_count);
    }
    return build_legacy_frame_features(raw_row, out_features, out_count);
}

static bool configure_model_input_buffers(size_t input_elements, int attempt)
{
    if (input_elements == 0) {
        publish_model_download_incompatible("input_elements_zero", input_elements);
        return false;
    }

    if (!is_supported_feature_layout(model_features_per_frame)) {
        publish_model_download_incompatible("unsupported_feature_layout", input_elements);
        return false;
    }

    if ((input_elements % model_features_per_frame) != 0) {
        publish_model_download_incompatible("input_elements_not_multiple_of_features", input_elements);
        return false;
    }

    size_t window_size = input_elements / model_features_per_frame;
    if (window_size == 0) {
        publish_model_download_incompatible("window_size_zero", input_elements);
        return false;
    }

    free_model_input_buffers();

    size_t bytes = input_elements * sizeof(float);
    model_window_ring = (float *)malloc(bytes);
    if (model_window_ring == NULL) {
        publish_model_download_memory_error("model",
                                            "window_ring_alloc_failed",
                                            HTTP_MODEL_URI,
                                            bytes,
                                            attempt,
                                            ESP_ERR_NO_MEM);
        free_model_input_buffers();
        return false;
    }

    model_input_scratch = (float *)malloc(bytes);
    if (model_input_scratch == NULL) {
        publish_model_download_memory_error("model",
                                            "window_scratch_alloc_failed",
                                            HTTP_MODEL_URI,
                                            bytes,
                                            attempt,
                                            ESP_ERR_NO_MEM);
        free_model_input_buffers();
        return false;
    }

    memset(model_window_ring, 0, bytes);
    memset(model_input_scratch, 0, bytes);
    model_input_elements = input_elements;
    model_window_size = window_size;
    model_window_fill = 0;
    model_window_head = 0;
    return true;
}

static bool mqtt_topic_equals(esp_mqtt_event_handle_t event, const char *topic)
{
    size_t expected_len = strlen(topic);
    return event->topic_len == (int)expected_len && strncmp(event->topic, topic, expected_len) == 0;
}

static void log_current_csi_row(const uint8_t *row, uint8_t count, int8_t rssi)
{
    if (row == NULL || count == 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((now - last_csi_log_us) < ((int64_t)CSI_LOG_MIN_INTERVAL_MS * 1000)) {
        return;
    }
    last_csi_log_us = now;

    // Only log a small slice of the buffer to keep serial output manageable.
    size_t show = count < 8 ? count : 8;
    ESP_LOGI(TAG, "CSI rssi=%d n=%u mean=%.1f max=%.1f first=%u,%u,%u,%u",
             rssi,
             count,
             last_csi_mean,
             last_csi_max,
             show > 0 ? row[0] : 0,
             show > 1 ? row[1] : 0,
             show > 2 ? row[2] : 0,
             show > 3 ? row[3] : 0);
}

static void reset_state_machine(void)
{
    for (int i = 0; i < 3; i++) {
        last_prediction_probs[i] = 0.3333f;
        smoothed_probs[i] = 0.3333f;
    }
    last_prediction_confidence = 0.0f;
    stable_state = -1;
    pending_state = -1;
    pending_count = 0;
    confidence_ema = 0.0f;
    entropy_ema = 0.0f;
    feature_abs_ema = 0.0f;
    drift_alert_streak = 0;
    last_drift_report_us = 0;
    last_uncertain_publish_us = 0;
    last_hold_publish_us = 0;
}

static float normalized_entropy(const float probs[3])
{
    if (probs == NULL) {
        return 1.0f;
    }

    float h = 0.0f;
    for (int i = 0; i < 3; i++) {
        float p = probs[i];
        if (p < 1e-6f) {
            continue;
        }
        h -= p * logf(p);
    }
    float max_h = logf(3.0f);
    if (max_h <= 0.0f) {
        return 1.0f;
    }
    float n = h / max_h;
    if (n < 0.0f) {
        return 0.0f;
    }
    if (n > 1.0f) {
        return 1.0f;
    }
    return n;
}

static void publish_uncertain_event_if_needed(int pred, float confidence, float entropy)
{
    if (confidence >= UNCERTAIN_CONFIDENCE_THRESHOLD) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((now - last_uncertain_publish_us) < ((int64_t)UNCERTAIN_PUBLISH_MIN_INTERVAL_MS * 1000)) {
        return;
    }
    last_uncertain_publish_us = now;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }

    cJSON_AddStringToObject(obj, "event", "inference_uncertain");
    cJSON_AddNumberToObject(obj, "pred", pred);
    cJSON_AddNumberToObject(obj, "confidence", confidence);
    cJSON_AddNumberToObject(obj, "entropy", entropy);
    cJSON_AddNumberToObject(obj, "confidence_ema", confidence_ema);
    cJSON_AddNumberToObject(obj, "entropy_ema", entropy_ema);

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);
}

static void publish_hold_event_if_needed(int stable_idx,
                                         int candidate_idx,
                                         float confidence,
                                         float entropy,
                                         const float probs[3])
{
    if (probs == NULL) {
        return;
    }
    if (stable_idx < 0 || stable_idx > 2 || candidate_idx < 0 || candidate_idx > 2) {
        return;
    }
    if (stable_idx == candidate_idx) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((now - last_hold_publish_us) < ((int64_t)HOLD_STATE_PUBLISH_MIN_INTERVAL_MS * 1000)) {
        return;
    }
    last_hold_publish_us = now;

    static const char *states[3] = {"door_open", "door_closed", "person_standing"};

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }

    cJSON_AddStringToObject(obj, "event", "state_hold");
    cJSON_AddStringToObject(obj, "held_state", states[stable_idx]);
    cJSON_AddStringToObject(obj, "candidate_state", states[candidate_idx]);
    cJSON_AddNumberToObject(obj, "confidence", confidence);
    cJSON_AddNumberToObject(obj, "entropy", entropy);
    cJSON_AddNumberToObject(obj, "p_open", probs[0]);
    cJSON_AddNumberToObject(obj, "p_closed", probs[1]);
    cJSON_AddNumberToObject(obj, "p_person", probs[2]);

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);
}

static void log_resolved_host(const char *url)
{
    const char *p = strstr(url, "://");
    if (p) {
        p += 3;
    } else {
        p = url;
    }

    const char *end = strchr(p, '/');
    size_t hostport_len = end ? (size_t)(end - p) : strlen(p);
    if (hostport_len == 0 || hostport_len >= 128) {
        return;
    }

    char hostport[128];
    strncpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    char host[128] = "";
    char port[16] = "";
    char *colon = strchr(hostport, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - hostport);
        if (hlen >= sizeof(host)) {
            hlen = sizeof(host) - 1;
        }
        memcpy(host, hostport, hlen);
        host[hlen] = '\0';
        strncpy(port, colon + 1, sizeof(port) - 1);
        port[sizeof(port) - 1] = '\0';
    } else {
        strncpy(host, hostport, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    if (host[0] == '\0') {
        return;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port[0] ? port : NULL, &hints, &res);
    if (err == 0 && res) {
        char ipstr[INET_ADDRSTRLEN];
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ipstr, sizeof(ipstr));
        ESP_LOGI(TAG, "Resolved host %s port=%s -> %s", host, port[0] ? port : "<none>", ipstr);
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "Failed to resolve host %s port=%s (getaddrinfo err=%d)", host, port[0] ? port : "<none>", err);
    }
}

static esp_err_t http_get_buffer(const char *url,
                                 uint8_t **out_buf,
                                 int *out_len,
                                 const char *asset,
                                 int attempt)
{
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 8000;

    esp_http_client_handle_t h = esp_http_client_init(&config);
    if (!h) {
        publish_model_download_memory_error(asset,
                                            "http_client_init_failed",
                                            url,
                                            0,
                                            attempt,
                                            ESP_ERR_NO_MEM);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http_get_buffer open failed (%s) for %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(h);
        return err;
    }

    uint32_t free_heap_before = esp_get_free_heap_size();
    uint32_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "http_get_buffer: free heap before download=%u largest_block=%u", free_heap_before, largest_before);

    int64_t content_len = esp_http_client_fetch_headers(h);
    if (content_len < 0) {
        ESP_LOGW(TAG, "http_get_buffer fetch_headers failed (%lld) for %s", content_len, url);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(h);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http_get_buffer bad HTTP status %d for %s", status, url);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    // Prefer exact allocation when content length is known to avoid realloc
    // fragmentation churn on low-memory devices.
    size_t alloc_len = (content_len > 0 && content_len < (1024 * 1024)) ? (size_t)(content_len + 1) : 2048;
    const size_t max_len = 1024 * 1024;

    if (content_len > 0) {
        uint32_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "http_get_buffer: status=%d content_len=%lld alloc_len=%u largest_block=%u",
                 status,
                 content_len,
                 (unsigned)alloc_len,
                 (unsigned)largest_now);
        if ((size_t)largest_now < alloc_len) {
            ESP_LOGW(TAG, "http_get_buffer: largest block too small for %s (need=%u, largest=%u)",
                     url,
                     (unsigned)alloc_len,
                     (unsigned)largest_now);
            publish_model_download_memory_error(asset,
                                                "largest_block_too_small",
                                                url,
                                                alloc_len,
                                                attempt,
                                                ESP_ERR_NO_MEM);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return ESP_ERR_NO_MEM;
        }
    }

    uint8_t *buf = (uint8_t *)malloc(alloc_len);
    if (!buf) {
        ESP_LOGW(TAG,
                 "http_get_buffer malloc(%u) failed, free heap=%u largest=%u",
                 (unsigned)alloc_len,
                 esp_get_free_heap_size(),
                 heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        publish_model_download_memory_error(asset,
                                            "malloc_failed",
                                            url,
                                            alloc_len,
                                            attempt,
                                            ESP_ERR_NO_MEM);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return ESP_ERR_NO_MEM;
    }

    const bool known_content_len = (content_len > 0 && content_len < (int64_t)max_len);
    int total_read = 0;
    while (true) {
        size_t space_left = alloc_len - (size_t)total_read - 1;
        if (space_left == 0) {
            if (known_content_len) {
                // For known Content-Length we allocated exact size (+NUL), so
                // hitting capacity means we've read the full body.
                break;
            }

            size_t new_len = alloc_len * 2;
            if (new_len > max_len) {
                ESP_LOGW(TAG, "http_get_buffer response too large for %s", url);
                publish_model_download_memory_error(asset,
                                                    "response_too_large",
                                                    url,
                                                    new_len,
                                                    attempt,
                                                    ESP_ERR_NO_MEM);
                free(buf);
                esp_http_client_close(h);
                esp_http_client_cleanup(h);
                return ESP_ERR_NO_MEM;
            }

            uint8_t *grown = (uint8_t *)realloc(buf, new_len);
            if (!grown) {
                ESP_LOGW(TAG,
                         "http_get_buffer realloc(%u) failed for %s, free=%u largest=%u",
                         (unsigned)new_len,
                         url,
                         esp_get_free_heap_size(),
                         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                publish_model_download_memory_error(asset,
                                                    "realloc_failed",
                                                    url,
                                                    new_len,
                                                    attempt,
                                                    ESP_ERR_NO_MEM);
                free(buf);
                esp_http_client_close(h);
                esp_http_client_cleanup(h);
                return ESP_ERR_NO_MEM;
            }

            buf = grown;
            alloc_len = new_len;
            space_left = alloc_len - (size_t)total_read - 1;
        }

        size_t read_cap = space_left;
        if (known_content_len) {
            size_t remaining = (size_t)content_len - (size_t)total_read;
            if (remaining == 0) {
                break;
            }
            if (read_cap > remaining) {
                read_cap = remaining;
            }
        }

        int read_len = esp_http_client_read(h,
                                            (char *)buf + total_read,
                                            (int)read_cap);
        if (read_len < 0) {
            ESP_LOGW(TAG, "http_get_buffer read failed (len=%d) for %s", read_len, url);
            free(buf);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        total_read += read_len;
    }

    esp_http_client_close(h);

    if (total_read <= 0) {
        ESP_LOGW(TAG, "http_get_buffer empty body (status=%d, content_len=%lld) for %s", status, content_len, url);
        free(buf);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    buf[total_read] = '\0';
    *out_buf = buf;
    *out_len = total_read;

    esp_http_client_cleanup(h);
    return ESP_OK;
}

static bool load_scaler_params(const uint8_t *json_buf, int json_len)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json_buf, json_len);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse scaler params");
        return false;
    }

    cJSON *mean_arr = cJSON_GetObjectItem(root, "mean");
    cJSON *std_arr = cJSON_GetObjectItem(root, "std");
    if (!cJSON_IsArray(mean_arr) || !cJSON_IsArray(std_arr)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "scaler params missing mean/std arrays");
        return false;
    }

    for (int i = 0; i < MAX_MODEL_FEATURES_PER_FRAME; i++) {
        mean_vals[i] = 0.0f;
        std_vals[i] = 1.0f;
    }

    int mean_sz = cJSON_GetArraySize(mean_arr);
    int std_sz = cJSON_GetArraySize(std_arr);
    if (mean_sz <= 0 || std_sz <= 0 || mean_sz != std_sz) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "scaler params mean/std mismatch mean=%d std=%d", mean_sz, std_sz);
        return false;
    }

    if (mean_sz > MAX_MODEL_FEATURES_PER_FRAME) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "scaler feature count too large: %d", mean_sz);
        return false;
    }

    int lim = mean_sz;
    for (int i = 0; i < lim; i++) {
        cJSON *jm = cJSON_GetArrayItem(mean_arr, i);
        cJSON *js = cJSON_GetArrayItem(std_arr, i);
        if (cJSON_IsNumber(jm)) {
            mean_vals[i] = (float)jm->valuedouble;
        }
        if (cJSON_IsNumber(js) && js->valuedouble != 0.0) {
            std_vals[i] = (float)js->valuedouble;
        }
    }

    notebook_mode_enabled = false;
    grouped_mode_enabled = false;
    grouped_feature_count = 0;
    selected_subcarrier_count = 0;
    notebook_extract_window_size = 5;
    notebook_calibration_frames = 20;
    notebook_use_session_offset = true;
    notebook_train_baseline = 1.0f;
    notebook_train_mean = 0.0f;

    cJSON *selected_sc = cJSON_GetObjectItem(root, "selected_subcarriers");
    if (cJSON_IsArray(selected_sc)) {
        int sc_count = cJSON_GetArraySize(selected_sc);
        if (sc_count > 0 && sc_count < MAX_MODEL_FEATURES_PER_FRAME) {
            bool valid = true;
            for (int i = 0; i < sc_count; i++) {
                cJSON *jsc = cJSON_GetArrayItem(selected_sc, i);
                if (!cJSON_IsNumber(jsc)) {
                    valid = false;
                    break;
                }

                int sc = (int)jsc->valuedouble;
                int raw_idx = raw_index_for_subcarrier(sc);
                if (raw_idx < 0 || raw_idx >= ACTIVE_SUBCARRIERS) {
                    valid = false;
                    break;
                }

                selected_subcarriers[i] = sc;
                selected_raw_indices[i] = raw_idx;
            }

            if (valid) {
                selected_subcarrier_count = (size_t)sc_count;
                notebook_mode_enabled = ((sc_count + 1) == lim);
            }
        }
    }

    cJSON *notebook_cfg = cJSON_GetObjectItem(root, "notebook_alignment");
    if (cJSON_IsObject(notebook_cfg)) {
        cJSON *j_window = cJSON_GetObjectItem(notebook_cfg, "extract_window_size");
        if (cJSON_IsNumber(j_window)) {
            int w = (int)j_window->valuedouble;
            if (w < 1) {
                w = 1;
            }
            if (w > NOTEBOOK_MAX_EXTRACT_WINDOW) {
                w = NOTEBOOK_MAX_EXTRACT_WINDOW;
            }
            notebook_extract_window_size = w;
        }

        cJSON *j_cal_frames = cJSON_GetObjectItem(notebook_cfg, "calibration_frames");
        if (cJSON_IsNumber(j_cal_frames)) {
            int cf = (int)j_cal_frames->valuedouble;
            if (cf < 1) {
                cf = 1;
            }
            notebook_calibration_frames = cf;
        }

        cJSON *j_baseline = cJSON_GetObjectItem(notebook_cfg, "train_baseline");
        if (cJSON_IsNumber(j_baseline)) {
            notebook_train_baseline = (float)j_baseline->valuedouble;
        }
        if (notebook_train_baseline <= FEATURE_EPSILON) {
            notebook_train_baseline = 1.0f;
        }

        cJSON *j_train_mean = cJSON_GetObjectItem(notebook_cfg, "train_mean");
        if (cJSON_IsNumber(j_train_mean)) {
            notebook_train_mean = (float)j_train_mean->valuedouble;
        }

        cJSON *j_use_offset = cJSON_GetObjectItem(notebook_cfg, "use_session_offset");
        if (cJSON_IsBool(j_use_offset)) {
            notebook_use_session_offset = cJSON_IsTrue(j_use_offset);
        }

        cJSON *j_feature_mode = cJSON_GetObjectItem(notebook_cfg, "feature_mode");
        if (cJSON_IsString(j_feature_mode) && j_feature_mode->valuestring &&
            strcmp(j_feature_mode->valuestring, "grouped") == 0) {
            int parsed_group_count = lim - 1;
            cJSON *j_group_count = cJSON_GetObjectItem(notebook_cfg, "group_count");
            if (cJSON_IsNumber(j_group_count)) {
                parsed_group_count = (int)j_group_count->valuedouble;
            }

            if (parsed_group_count > 0 && (parsed_group_count + 1) == lim) {
                grouped_mode_enabled = true;
                grouped_feature_count = (size_t)parsed_group_count;
                notebook_mode_enabled = false;
            }
        }
    }

    if (!notebook_mode_enabled &&
        !grouped_mode_enabled &&
        lim != COMPACT_FEATURE_COUNT &&
        lim != LEGACY_RAW55_FEATURE_COUNT &&
        lim != LEGACY_RAW56_FEATURE_COUNT) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "unsupported scaler feature layout: %d", lim);
        return false;
    }

    model_features_per_frame = (size_t)lim;
    model_input_elements = model_features_per_frame;
    notebook_ring_count = 0;
    notebook_ring_head = 0;
    notebook_session_mean_accum = 0.0f;
    notebook_session_mean_count = 0;
    notebook_variation_baseline_accum = 0.0f;
    notebook_variation_baseline_count = 0;

    ESP_LOGI(TAG,
             "Loaded scaler: feature_count=%d mode=%s selected_subcarriers=%u grouped_features=%u extract_window=%d calib_frames=%d",
             lim,
             grouped_mode_enabled ? "grouped" : (notebook_mode_enabled ? "notebook" : "legacy"),
             (unsigned)selected_subcarrier_count,
             (unsigned)grouped_feature_count,
             notebook_extract_window_size,
             notebook_calibration_frames);

    cJSON_Delete(root);
    return true;
}

static int run_model(const float *input, size_t len, float out_probs[3], float *out_confidence)
{
    static uint32_t inference_profile_log_counter = 0;

    if (!model_ready || !input || len == 0) {
        return -1;
    }

    float confidence = 0.0f;
    float probs[3] = {0.0f, 0.0f, 0.0f};
    int pred = tflm_predict_with_probs(input, len, probs, 3, &confidence);
    if (pred < 0 || pred > 2) {
        ESP_LOGW(TAG, "tflm_predict failed: %s", tflm_last_error());
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        last_prediction_probs[i] = probs[i];
        if (out_probs != NULL) {
            out_probs[i] = probs[i];
        }
    }

    last_prediction_confidence = confidence;
    if (out_confidence != NULL) {
        *out_confidence = confidence;
    }

    inference_profile_log_counter++;
    if ((inference_profile_log_counter % PASO_PERF_PUBLISH_INTERVAL_SAMPLES) == 0) {
        tflm_inference_profile_t prof = {};
        if (tflm_get_inference_profile(&prof)) {
            ESP_LOGI(TAG,
                     "TFLM invoke_us last=%" PRIu32 " avg=%" PRIu32 " min=%" PRIu32 " max=%" PRIu32 " n=%" PRIu32,
                     prof.last_us,
                     prof.avg_us,
                     prof.min_us,
                     prof.max_us,
                     prof.sample_count);
            if (prof.avg_us > PASO_INFERENCE_BUDGET_US) {
                ESP_LOGW(TAG,
                         "PASO budget exceed: invoke avg=%" PRIu32 "us > %dus",
                         prof.avg_us,
                         PASO_INFERENCE_BUDGET_US);
            }
        }
    }

    return pred;
}

static void publish_state_change(int state_idx)
{
    if (state_idx < 0 || state_idx > 2) {
        return;
    }

    static const char *states[3] = {"door_open", "door_closed", "person_standing"};

    ESP_LOGI(TAG,
             "state_change => %s (conf=%.2f, smooth=[%.2f, %.2f, %.2f])",
             states[state_idx],
             last_prediction_confidence,
             smoothed_probs[0],
             smoothed_probs[1],
             smoothed_probs[2]);

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", "state_change");
    cJSON_AddStringToObject(obj, "state", states[state_idx]);
    cJSON_AddNumberToObject(obj, "confidence", last_prediction_confidence);
    cJSON_AddNumberToObject(obj, "p_open", last_prediction_probs[0]);
    cJSON_AddNumberToObject(obj, "p_closed", last_prediction_probs[1]);
    cJSON_AddNumberToObject(obj, "p_person", last_prediction_probs[2]);
    cJSON_AddNumberToObject(obj, "smooth_open", smoothed_probs[0]);
    cJSON_AddNumberToObject(obj, "smooth_closed", smoothed_probs[1]);
    cJSON_AddNumberToObject(obj, "smooth_person", smoothed_probs[2]);

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);
}

static void maybe_publish_drift_event(int pred)
{
    int64_t now = esp_timer_get_time();
    bool periodic_due = (last_drift_report_us == 0) ||
                        ((now - last_drift_report_us) >= ((int64_t)DRIFT_REPORT_INTERVAL_MS * 1000));

    bool drift_alert = ((entropy_ema >= DRIFT_ENTROPY_THRESHOLD) &&
                        (confidence_ema <= DRIFT_CONFIDENCE_THRESHOLD)) ||
                       (feature_abs_ema >= DRIFT_FEATURE_ABS_THRESHOLD);
    if (drift_alert) {
        drift_alert_streak++;
    } else if (drift_alert_streak > 0) {
        drift_alert_streak--;
    }

    bool alert_due = drift_alert && (drift_alert_streak >= DRIFT_ALERT_CONSECUTIVE);
    if (!periodic_due && !alert_due) {
        return;
    }

    last_drift_report_us = now;
    const char *event_name = alert_due ? "drift_alert" : "drift_report";

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", event_name);
    cJSON_AddNumberToObject(obj, "pred", pred);
    cJSON_AddNumberToObject(obj, "stable_state", stable_state);
    cJSON_AddNumberToObject(obj, "confidence_ema", confidence_ema);
    cJSON_AddNumberToObject(obj, "entropy_ema", entropy_ema);
    cJSON_AddNumberToObject(obj, "feature_abs_ema", feature_abs_ema);
    cJSON_AddBoolToObject(obj, "calibration_drift_suspected", drift_alert ? 1 : 0);

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);

    if (alert_due) {
        drift_alert_streak = 0;
    }
}

static void update_state_machine_and_notify(int pred, const float probs[3], float confidence, float feature_abs_mean)
{
    if (pred < 0 || pred > 2 || probs == NULL) {
        return;
    }

    float entropy = normalized_entropy(probs);
    const float ema_alpha = 0.90f;
    confidence_ema = (confidence_ema == 0.0f) ? confidence : (ema_alpha * confidence_ema + (1.0f - ema_alpha) * confidence);
    entropy_ema = (entropy_ema == 0.0f) ? entropy : (ema_alpha * entropy_ema + (1.0f - ema_alpha) * entropy);
    feature_abs_ema = (feature_abs_ema == 0.0f) ? feature_abs_mean : (ema_alpha * feature_abs_ema + (1.0f - ema_alpha) * feature_abs_mean);

    float sum_smooth = 0.0f;
    for (int i = 0; i < 3; i++) {
        smoothed_probs[i] = (STATE_SMOOTHING_ALPHA * smoothed_probs[i]) + ((1.0f - STATE_SMOOTHING_ALPHA) * probs[i]);
        if (smoothed_probs[i] < 0.0f) {
            smoothed_probs[i] = 0.0f;
        }
        sum_smooth += smoothed_probs[i];
    }
    if (sum_smooth > 0.0f) {
        for (int i = 0; i < 3; i++) {
            smoothed_probs[i] /= sum_smooth;
        }
    }

    int best_idx = 0;
    if (smoothed_probs[1] > smoothed_probs[best_idx]) {
        best_idx = 1;
    }
    if (smoothed_probs[2] > smoothed_probs[best_idx]) {
        best_idx = 2;
    }

    float best_score = smoothed_probs[best_idx];
    float stable_score = (stable_state >= 0 && stable_state < 3) ? smoothed_probs[stable_state] : 0.0f;
    bool uncertain = confidence < UNCERTAIN_CONFIDENCE_THRESHOLD;

    publish_uncertain_event_if_needed(pred, confidence, entropy);
    if (uncertain && stable_state >= 0 && best_idx != stable_state) {
        publish_hold_event_if_needed(stable_state, best_idx, confidence, entropy, probs);
    }

    if (stable_state < 0) {
        if (!uncertain && best_score >= STATE_ENTER_THRESHOLD) {
            if (pending_state == best_idx) {
                pending_count++;
            } else {
                pending_state = best_idx;
                pending_count = 1;
            }
            if (pending_count >= STATE_SWITCH_CONSECUTIVE) {
                stable_state = best_idx;
                pending_state = -1;
                pending_count = 0;
                publish_state_change(stable_state);
            }
        } else {
            pending_state = -1;
            pending_count = 0;
        }
        maybe_publish_drift_event(pred);
        return;
    }

    if (best_idx == stable_state) {
        pending_state = -1;
        pending_count = 0;
        maybe_publish_drift_event(pred);
        return;
    }

    bool switch_candidate = (!uncertain) &&
                            (best_score >= STATE_ENTER_THRESHOLD) &&
                            (stable_score <= STATE_EXIT_THRESHOLD);

    if (switch_candidate) {
        if (pending_state == best_idx) {
            pending_count++;
        } else {
            pending_state = best_idx;
            pending_count = 1;
        }

        if (pending_count >= STATE_SWITCH_CONSECUTIVE) {
            stable_state = best_idx;
            pending_state = -1;
            pending_count = 0;
            publish_state_change(stable_state);
        }
    } else {
        pending_state = -1;
        pending_count = 0;
    }

    maybe_publish_drift_event(pred);
}

static void vInferenceTask(void *pvParameters)
{
    (void)pvParameters;

    inference_sample_t sample;
    uint32_t inference_stage_log_counter = 0;
    for (;;) {
        if (inference_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int64_t dequeue_wait_start_us = esp_timer_get_time();
        BaseType_t recv_status = xQueueReceive(inference_queue, &sample, pdMS_TO_TICKS(1000));
        int64_t dequeue_wait_us = esp_timer_get_time() - dequeue_wait_start_us;

        if (recv_status == pdPASS) {
            int64_t pipeline_start_us = esp_timer_get_time();

            if (!model_ready) {
                continue;
            }

            if (model_window_ring == NULL ||
                model_input_scratch == NULL ||
                model_window_size == 0 ||
                model_input_elements == 0 ||
                model_features_per_frame == 0 ||
                model_features_per_frame > MAX_MODEL_FEATURES_PER_FRAME) {
                continue;
            }

            float frame_input[MAX_MODEL_FEATURES_PER_FRAME];
            int64_t feature_build_start_us = esp_timer_get_time();
            if (!build_frame_features(sample.normalized, frame_input, model_features_per_frame)) {
                continue;
            }
            int64_t feature_build_us = esp_timer_get_time() - feature_build_start_us;

            float feature_abs_mean = 0.0f;
            for (size_t i = 0; i < model_features_per_frame; i++) {
                float sigma = std_vals[i];
                if (sigma == 0.0f) {
                    sigma = 1.0f;
                }
                frame_input[i] = (frame_input[i] - mean_vals[i]) / sigma;
                feature_abs_mean += fabsf(frame_input[i]);
            }
            feature_abs_mean /= (float)model_features_per_frame;

            size_t write_offset = model_window_head * model_features_per_frame;
            memcpy(model_window_ring + write_offset,
                   frame_input,
                   model_features_per_frame * sizeof(float));

            model_window_head = (model_window_head + 1) % model_window_size;
            if (model_window_fill < model_window_size) {
                model_window_fill++;
            }

            if (model_window_fill < model_window_size) {
                continue;
            }

            for (size_t w = 0; w < model_window_size; w++) {
                size_t frame_idx = (model_window_head + w) % model_window_size;
                                memcpy(model_input_scratch + (w * model_features_per_frame),
                                             model_window_ring + (frame_idx * model_features_per_frame),
                                             model_features_per_frame * sizeof(float));
            }

            float probs[3] = {0.0f, 0.0f, 0.0f};
            float confidence = 0.0f;
            int64_t invoke_stage_start_us = esp_timer_get_time();
            int pred = run_model(model_input_scratch, model_input_elements, probs, &confidence);
            int64_t invoke_stage_us = esp_timer_get_time() - invoke_stage_start_us;
            update_state_machine_and_notify(pred, probs, confidence, feature_abs_mean);

            int64_t pipeline_total_us = esp_timer_get_time() - pipeline_start_us;
            inference_stage_log_counter++;
            if ((inference_stage_log_counter % PASO_PERF_PUBLISH_INTERVAL_SAMPLES) == 0) {
                ESP_LOGI(TAG,
                         "Inference stage_us wait=%" PRIi64 " feature=%" PRIi64 " invoke=%" PRIi64 " total=%" PRIi64,
                         dequeue_wait_us,
                         feature_build_us,
                         invoke_stage_us,
                         pipeline_total_us);
                publish_perf_binary_metrics(dequeue_wait_us,
                                            feature_build_us,
                                            invoke_stage_us,
                                            pipeline_total_us);
                if (pipeline_total_us > PASO_PIPELINE_BUDGET_US) {
                    ESP_LOGW(TAG,
                             "PASO budget exceed: pipeline total=%" PRIi64 "us > %dus",
                             pipeline_total_us,
                             PASO_PIPELINE_BUDGET_US);
                }
            }

            // Yield to keep the networking tasks responsive.
            taskYIELD();
        }
    }
}

static void download_model_and_params_task(void *pvParameters)
{
    (void)pvParameters;

    // Remove any previously loaded model before downloading a new one. This
    // frees RAM and avoids leaving stale model state in place.
    if (model_buffer) {
        free(model_buffer);
        model_buffer = NULL;
        free_model_input_buffers();
        model_size = 0;
        model_ready = false;
        tflm_reset();
        ESP_LOGI(TAG, "Cleared existing model from RAM before downloading a new one");
        notify_status_task();
    }

    for (int attempt = 1; attempt <= 3; attempt++) {
        uint8_t *downloaded_model = NULL;
        uint8_t *downloaded_params = NULL;
        int model_len = 0;
        int params_len = 0;
        bool scaler_ok = false;

        // If an upload HTTP client is still around, release it to maximize
        // available heap before model/scaler download.
        if (client != NULL) {
            esp_http_client_cleanup(client);
            client = NULL;
        }

        ESP_LOGI(TAG, "Model download attempt %d/3: free heap=%u fetching %s and %s", attempt, esp_get_free_heap_size(), HTTP_MODEL_URI, HTTP_PARAMS_URI);
        log_resolved_host(HTTP_MODEL_URI);
        log_resolved_host(HTTP_PARAMS_URI);

        // Download and parse scaler first, then free it before model download
        // to lower peak RAM pressure.
        esp_err_t e2 = http_get_buffer(HTTP_PARAMS_URI, &downloaded_params, &params_len, "scaler", attempt);
        if (e2 == ESP_OK) {
            scaler_ok = load_scaler_params(downloaded_params, params_len);
            free(downloaded_params);
            downloaded_params = NULL;
        }

        esp_err_t e1 = http_get_buffer(HTTP_MODEL_URI, &downloaded_model, &model_len, "model", attempt);

        ESP_LOGI(TAG,
                 "Download results: model err=%s len=%d, params err=%s len=%d scaler_ok=%d largest_block=%u",
                 esp_err_to_name(e1),
                 model_len,
                 esp_err_to_name(e2),
                 params_len,
                 scaler_ok,
                 heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        if (e1 == ESP_OK && e2 == ESP_OK) {
            bool model_ok = tflm_load_model(downloaded_model, (size_t)model_len);
            if (!scaler_ok) {
                ESP_LOGW(TAG, "Scaler params load failed (len=%d)", params_len);
            }
            if (!model_ok) {
                ESP_LOGW(TAG, "TFLM model load failed (len=%d): %s", model_len, tflm_last_error());
                publish_model_download_incompatible(tflm_last_error(), 0);
            }
            size_t input_elements = tflm_input_element_count();
            bool input_buffers_ok = false;
            if (model_ok) {
                input_buffers_ok = configure_model_input_buffers(input_elements, attempt);
                if (!input_buffers_ok) {
                    ESP_LOGW(TAG,
                             "Model input buffer configuration failed (elements=%u features_per_frame=%u remainder=%u)",
                             (unsigned)input_elements,
                             (unsigned)model_features_per_frame,
                             (unsigned)(input_elements % model_features_per_frame));
                    tflm_reset();
                    model_ok = false;
                }
            }
            if (scaler_ok && model_ok && input_buffers_ok) {
                if (model_buffer) {
                    free(model_buffer);
                }
                model_buffer = downloaded_model;
                model_size = (size_t)model_len;
                model_ready = true;
                notify_status_task();
                publish_status("model_ready", true, true);  // ensure dashboard sees model load immediately
                reset_state_machine();
                model_window_fill = 0;
                model_window_head = 0;

                cJSON *obj = cJSON_CreateObject();
                if (obj) {
                    cJSON_AddStringToObject(obj, "event", "model_ready");
                    cJSON_AddNumberToObject(obj, "bytes", (double)model_size);
                    cJSON_AddNumberToObject(obj, "input_elements", (double)model_input_elements);
                    cJSON_AddNumberToObject(obj, "window_size", (double)model_window_size);
                    char *json_str = cJSON_PrintUnformatted(obj);
                    if (json_str) {
                        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
                        free(json_str);
                    }
                    cJSON_Delete(obj);
                }

                ESP_LOGI(TAG, "Model/scaler downloaded (%d bytes)", model_len);
                model_download_in_progress = false;
                vTaskDelete(NULL);
                return;
            }

            ESP_LOGW(TAG, "Downloaded model/scaler invalid (scaler_ok=%d model_ok=%d): %s",
                     scaler_ok, model_ok, tflm_last_error());
        }

        if (downloaded_model) {
            free(downloaded_model);
        }
        if (downloaded_params) {
            free(downloaded_params);
        }

        ESP_LOGW(TAG, "Model/scaler download attempt %d/3 failed", attempt);
        vTaskDelay(pdMS_TO_TICKS(backoff_jitter_ms(attempt - 1, 500)));
    }

    model_ready = false;
    free_model_input_buffers();
    notify_status_task();
    tflm_reset();

    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddStringToObject(obj, "event", "model_download_failed");
        char *json_str = cJSON_PrintUnformatted(obj);
        if (json_str) {
            publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
            free(json_str);
        }
        cJSON_Delete(obj);
    }

    model_download_in_progress = false;
    vTaskDelete(NULL);
}

static void free_http_task_params(http_task_params_t *params)
{
    if (params == NULL) {
        return;
    }
    if (params->payload != NULL) {
        free(params->payload);
        params->payload = NULL;
    }
    if (params->label != NULL) {
        free(params->label);
        params->label = NULL;
    }
    free(params);
}

static bool is_http_success_status(int status_code)
{
    return status_code >= 200 && status_code < 300;
}

static bool should_retry_upload_attempt(esp_err_t err, int status_code)
{
    if (err != ESP_OK) {
        return true;
    }

    // Explicitly retry CRC mismatch responses to recover from transient corruption.
    if (status_code == 422) {
        return true;
    }

    if (status_code == 408 || status_code == 429) {
        return true;
    }

    if (status_code >= 500) {
        return true;
    }

    return false;
}

static void send_batch_http_task(void *pvParameters)
{
    if (pvParameters == NULL) {
        ESP_LOGE(TAG, "HTTP task received NULL parameters");
        vTaskDelete(NULL);
        return;
    }

    http_task_params_t *params = (http_task_params_t *)pvParameters;
    if (mqtt_client == NULL || params->label == NULL || params->payload == NULL || params->payload_len == 0) {
        ESP_LOGE(TAG, "HTTP task invalid state (mqtt=%p label=%p payload=%p len=%u)",
                 (void *)mqtt_client,
                 (void *)params->label,
                 (void *)params->payload,
                 (unsigned)params->payload_len);
        free_http_task_params(params);
        vTaskDelete(NULL);
        return;
    }

    // Progress tracking headers for server-side assembly of batches
    char sub_batch_str[6];
    snprintf(sub_batch_str, sizeof(sub_batch_str), "%d", params->sub_batch_idx);

    char total_batches_str[6];
    snprintf(total_batches_str, sizeof(total_batches_str), "%d", params->total_sub_batches);

    // CRC32 is 32 bits; formatted as 8 hex digits plus null terminator.
    char crc32_str[9];
    snprintf(crc32_str, sizeof(crc32_str), "%08" PRIx32, params->payload_crc32);

    // Idempotency key: session + sub-batch index (content changes are caught by CRC)
    // Use the per-task session snapshot so retries cannot drift if globals change.
    char idem_key[IDEMPOTENCY_KEY_MAX_LEN];
    const char *session_for_headers = (params->session_id[0] != '\0') ? params->session_id : "nosession";
    snprintf(idem_key, sizeof(idem_key), "%s_%d", session_for_headers, params->sub_batch_idx);

    const int max_attempts = 4; // initial + 3 retries
    bool upload_success = false;
    esp_err_t last_err = ESP_FAIL;
    int last_http_status = -1;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        esp_http_client_config_t config = {};
        config.url = HTTP_UPLOAD_URI;
        config.method = HTTP_METHOD_POST;

        esp_http_client_handle_t http_client = esp_http_client_init(&config);
        if (http_client == NULL) {
            ESP_LOGE(TAG, "Failed to init HTTP client for upload attempt %d/%d", attempt, max_attempts);
            last_err = ESP_FAIL;
            last_http_status = -1;
            break;
        }

        // Tell server the label and metadata for this batch in HTTP headers
        esp_http_client_set_header(http_client, "X-Room-State", params->label);
        esp_http_client_set_header(http_client, "X-ESP32-ID", CONFIG_CSI_NODE_ID);
        if (params->session_id[0] != '\0') {
            esp_http_client_set_header(http_client, "X-Session-ID", params->session_id);
        }
        esp_http_client_set_header(http_client, "X-Sub-Batch-Index", sub_batch_str);
        esp_http_client_set_header(http_client, "X-Total-Sub-Batches", total_batches_str);
        esp_http_client_set_header(http_client, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(http_client, "X-CRC32", crc32_str);
        esp_http_client_set_header(http_client, "X-Idempotency-Key", idem_key);
        esp_http_client_set_post_field(http_client, (const char *)params->payload, (int)params->payload_len);

        uint32_t heap_before = esp_get_free_heap_size();
        uint32_t min_heap_before = esp_get_minimum_free_heap_size();

        last_err = esp_http_client_perform(http_client);
        last_http_status = (last_err == ESP_OK) ? esp_http_client_get_status_code(http_client) : -1;

        uint32_t heap_after = esp_get_free_heap_size();
        uint32_t min_heap_after = esp_get_minimum_free_heap_size();

        if (last_err == ESP_OK && is_http_success_status(last_http_status)) {
            ESP_LOGI(TAG,
                     "Batch uploaded successfully (%d/%d), HTTP %d (attempt %d/%d) - CSI mean=%.1f max=%.1f",
                     params->sub_batch_idx + 1,
                     params->total_sub_batches,
                     last_http_status,
                     attempt,
                     max_attempts,
                     last_csi_mean,
                     last_csi_max);
            ESP_LOGW("MEM_STATS", "HTTP call stats: used %lu bytes (peak %lu during call)",
                     (unsigned long)(heap_before - heap_after),
                     (unsigned long)(min_heap_before - min_heap_after));

            char progress[128];
            snprintf(progress, sizeof(progress),
                     "{\"event\":\"upload\",\"sub\":%d,\"total\":%d,\"csi_mean\":%.1f,\"csi_max\":%.1f}",
                     params->sub_batch_idx + 1,
                     params->total_sub_batches,
                     last_csi_mean,
                     last_csi_max);
            publish_with_retry(ESP32_STATUS_TOPIC, progress, 1);

            upload_success = true;
            esp_http_client_cleanup(http_client);
            break;
        }

        bool retryable = should_retry_upload_attempt(last_err, last_http_status);
        if (last_http_status == 422) {
            ESP_LOGW(TAG,
                     "HTTP 422 (CRC mismatch) for sub-batch %d on attempt %d/%d; retrying",
                     params->sub_batch_idx + 1,
                     attempt,
                     max_attempts);
        } else {
            ESP_LOGW(TAG,
                     "HTTP upload failed for sub-batch %d (attempt %d/%d): err=%s status=%d retryable=%d",
                     params->sub_batch_idx + 1,
                     attempt,
                     max_attempts,
                     esp_err_to_name(last_err),
                     last_http_status,
                     retryable ? 1 : 0);
        }

        char failmsg[192];
        snprintf(failmsg, sizeof(failmsg),
                 "{\"event\":\"upload_failed\",\"sub\":%d,\"attempt\":%d,\"status\":%d,\"err\":%d}",
                 params->sub_batch_idx + 1,
                 attempt,
                 last_http_status,
                 last_err);
        publish_with_retry(ESP32_STATUS_TOPIC, failmsg, 1);

        esp_http_client_cleanup(http_client);

        if (!retryable || attempt == max_attempts) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(backoff_jitter_ms(attempt - 1, 500)));
    }

    if (!upload_success) {
        ESP_LOGE(TAG,
                 "Final upload failure for sub-batch %d after retries (err=%s status=%d); aborting collection",
                 params->sub_batch_idx + 1,
                 esp_err_to_name(last_err),
                 last_http_status);
        is_collecting = false;
    }

    // Firmware fallback completion event after final sub-batch upload
    if (upload_success && params->sub_batch_idx == params->total_sub_batches - 1) {
        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj) {
            cJSON_AddStringToObject(msg_obj, "event", "collection_complete");
            cJSON_AddStringToObject(msg_obj, "label", params->label);
            if (params->session_id[0] != '\0') {
                cJSON_AddStringToObject(msg_obj, "session", params->session_id);
            }
            cJSON_AddStringToObject(msg_obj, "source", "firmware");
            char *json_str = cJSON_PrintUnformatted(msg_obj);
            if (json_str) {
                publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
                free(json_str);
            }
            cJSON_Delete(msg_obj);
        }
        ESP_LOGI(TAG, "Free Heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    }

    free_http_task_params(params);

    vTaskDelete(NULL);
}

static void wifi_csi_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;

    if (wifi_connected == false || info == NULL) {
        return;
    }

    // In idle mode, keep router packets only. During collection, allow ambient
    // real traffic to improve sample availability without generating dummy packets.
    if (!is_collecting && memcmp(info->mac, dynamic_target_mac, 6) != 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t dt = now - last_trigger_us;

    // Only accept one packet per expected period
    if (dt < EXPECTED_PERIOD_US) {
        return;
    }

    // Filter out outlier packets for consistent CSI data
    if (info->rx_ctrl.sig_len > 32 || info->rx_ctrl.rssi < -100 || info->rx_ctrl.rx_state != 0) {
        return;
    }

    last_trigger_us = now;

    // Extract raw data
    int8_t *raw = (int8_t *)info->buf;
    uint16_t current_pwrs[ACTIVE_SUBCARRIERS];
    uint32_t sum_pwr = 0;
    uint8_t valid_sc_count = 0;

    // Normalization pass 1: calculate powers and packet average
    for (uint8_t sc = MAX_LOWER; sc <= MAX_UPPER; sc++) {
        if (sc == DC_NULL) {
            continue;
        }

        int8_t i = raw[sc * 2];
        int8_t q = raw[sc * 2 + 1];
        uint16_t pwr = (uint16_t)(i * i + q * q);

        if (valid_sc_count < ACTIVE_SUBCARRIERS) {
            current_pwrs[valid_sc_count] = pwr;
            sum_pwr += pwr;
            valid_sc_count++;
        }
    }

    if (valid_sc_count == 0 || sum_pwr == 0) {
        return;
    }

    // Normalization pass 2: feature vector
    uint8_t normalized_row[ACTIVE_SUBCARRIERS] = {0};
    uint32_t multiplier = (100 * valid_sc_count << 8) / sum_pwr;

    float sum_norm = 0.0f;
    uint8_t max_norm = 0;
    for (uint8_t i = 0; i < valid_sc_count; i++) {
        uint32_t normalized = (current_pwrs[i] * multiplier) >> 8;
        uint8_t v = (uint8_t)(normalized > 255 ? 255 : normalized);
        normalized_row[i] = v;
        sum_norm += (float)v;
        if (v > max_norm) {
            max_norm = v;
        }
    }

    // Keep a running summary of the latest CSI packet values
    if (valid_sc_count > 0) {
        last_csi_mean = sum_norm / (float)valid_sc_count;
        last_csi_max = (float)max_norm;
    }

    // Avoid high-rate serial logs while idle; keep verbose CSI logs only when
    // actively collecting or inferring.
    if (is_collecting || model_ready) {
        log_current_csi_row(normalized_row, valid_sc_count, info->rx_ctrl.rssi);
    }

    // --- PATH A: INFERENCE (offloaded to a separate task to avoid blocking the Wi‑Fi callback) ---
    if (model_ready && inference_queue) {
        inference_sample_t sample;
        memcpy(sample.normalized, normalized_row, ACTIVE_SUBCARRIERS);
        // Non-blocking enqueue: if queue is full, drop and track the rate.
        BaseType_t queued = xQueueSendToBack(inference_queue, &sample, 0);
        if (queued != pdPASS) {
            inference_drop_count++;
            if ((inference_drop_count % INFERENCE_DROP_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG,
                         "Inference queue full: dropped=%" PRIu32 " (queue_depth=%d waiting=%u)",
                         inference_drop_count,
                         INFERENCE_QUEUE_DEPTH,
                         (unsigned)uxQueueMessagesWaiting(inference_queue));
            }
        }
    }

    // --- PATH B: DATA COLLECTION (gated by MQTT command) ---
    if (is_collecting) {
        if (packet_idx < SUB_BATCH_SIZE) {
            memcpy(csi_buffer[packet_idx], normalized_row, sizeof(normalized_row));
            packet_idx++;
        }

        if (packet_idx >= SUB_BATCH_SIZE) {
            ESP_LOGI(TAG, "Sub-batch full (%d/%d). Triggering upload for state: %s",
                     sub_batch_idx + 1,
                     (SAMPLE_SIZE / SUB_BATCH_SIZE),
                     collection_label);

            http_task_params_t *params = (http_task_params_t *)malloc(sizeof(http_task_params_t));
            if (params != NULL) {
                memset(params, 0, sizeof(*params));
                params->label = strdup(collection_label);
                params->sub_batch_idx = sub_batch_idx;
                params->total_sub_batches = (SAMPLE_SIZE / SUB_BATCH_SIZE);
                snprintf(params->session_id, sizeof(params->session_id), "%s", current_session_id);

                params->payload_len = sizeof(csi_buffer);
                params->payload = (uint8_t *)malloc(params->payload_len);

                if (params->label == NULL || params->payload == NULL) {
                    ESP_LOGE(TAG,
                             "Failed to allocate HTTP payload snapshot (label=%p payload=%p len=%u)",
                             (void *)params->label,
                             (void *)params->payload,
                             (unsigned)params->payload_len);
                    free_http_task_params(params);
                    params = NULL;
                }

                if (params != NULL) {
                    memcpy(params->payload, csi_buffer, params->payload_len);
                    params->payload_crc32 = esp_crc32_le(0, params->payload, params->payload_len);
                }

                if (params != NULL) {
                    if (xTaskCreate(send_batch_http_task,
                                    "http_batch_upload",
                                    8192,
                                    params,
                                    5,
                                    NULL) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to create HTTP upload task");
                        free_http_task_params(params);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for HTTP task parameters");
            }

            packet_idx = 0;
            sub_batch_idx++;

            if (sub_batch_idx >= (SAMPLE_SIZE / SUB_BATCH_SIZE)) {
                sub_batch_idx = 0;
                is_collecting = false;
                ESP_LOGI(TAG, "Collection complete for state: %s", collection_label);
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected (broker=%s)", MQTT_BROKER_URI);
        int lwt_online_msg_id = esp_mqtt_client_publish(event->client,
                                                        ESP32_DEVICE_STATUS_TOPIC,
                                                        "online",
                                                        6,
                                                        1,
                                                        1);
        if (lwt_online_msg_id < 0) {
            ESP_LOGW(TAG, "Failed to publish LWT online message topic=%s", ESP32_DEVICE_STATUS_TOPIC);
        }
        esp_mqtt_client_subscribe(event->client, ESP32_COLLECT_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, ESP32_DOWNLOAD_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, ESP32_TRAINING_COMPLETE_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, ESP32_LOAD_MODEL_TOPIC, 1);
        mqtt_client = event->client;
        mqtt_connected = true;
        // Notify the dashboard that we are online and include model status.
        publish_status("node_online", true, true);
        notify_status_task();

        // Auto-reload model on reconnect (since RAM is cleared on reset)
        if (auto_load_model_on_reconnect && !model_ready && !model_download_in_progress) {
            ESP_LOGI(TAG, "Auto-loading model on MQTT reconnect");
            model_download_in_progress = true;
            if (xTaskCreate(download_model_and_params_task,
                            "model_dl_auto",
                            8192,
                            NULL,
                            5,
                            NULL) != pdPASS) {
                model_download_in_progress = false;
                ESP_LOGE(TAG, "Failed to create auto model download task");
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT Disconnected");
        mqtt_connected = false;
        // Avoid publish attempts while disconnected; broker LWT advertises offline.
        mqtt_client = NULL;
        notify_status_task();
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received data on topic: %.*s (qos=%d retain=%d dup=%d)",
                 event->topic_len,
                 event->topic,
                 event->qos,
                 event->retain,
                 event->dup);

        if (mqtt_topic_equals(event, ESP32_COLLECT_TOPIC)) {
            publish_ack("collect");

            // Payload may be raw label or JSON {"label":"...","session":"..."}
            char payload_buf[256] = {0};
            snprintf(payload_buf, sizeof(payload_buf), "%.*s", event->data_len, event->data);

            collection_label[0] = '\0';
            current_session_id[0] = '\0';

            cJSON *root = cJSON_Parse(payload_buf);
            if (root && cJSON_IsObject(root)) {
                cJSON *jlabel = cJSON_GetObjectItem(root, "label");
                if (cJSON_IsString(jlabel) && jlabel->valuestring) {
                    snprintf(collection_label, sizeof(collection_label), "%s", jlabel->valuestring);
                }

                cJSON *jsession = cJSON_GetObjectItem(root, "session");
                if (cJSON_IsString(jsession) && jsession->valuestring) {
                    snprintf(current_session_id, sizeof(current_session_id), "%s", jsession->valuestring);
                }
            }
            cJSON_Delete(root);

            if (!collection_label[0]) {
                snprintf(collection_label, sizeof(collection_label), "%.*s", event->data_len, event->data);
            }
            if (!current_session_id[0]) {
                int64_t t = esp_timer_get_time();
                snprintf(current_session_id, sizeof(current_session_id), "%lld", t);
            }

            packet_idx = 0;
            sub_batch_idx = 0;
            is_collecting = true;
            reset_state_machine();

            ESP_LOGI(TAG, "Starting data collection for state: %s (session=%s)",
                     collection_label,
                     current_session_id);
        } else if (mqtt_topic_equals(event, ESP32_TRAINING_COMPLETE_TOPIC)) {
            publish_ack("training_complete");
            model_download_armed = true;
            notify_status_task();

            if (model_download_in_progress) {
                ESP_LOGW(TAG, "Ignoring training_complete: model download already in progress");
            } else {
                ESP_LOGI(TAG, "Model download requested (training_complete)");
                model_download_in_progress = true;
                if (xTaskCreate(download_model_and_params_task,
                                "model_dl",
                                8192,
                                NULL,
                                5,
                                NULL) != pdPASS) {
                    model_download_in_progress = false;
                    ESP_LOGE(TAG, "Failed to create model download task");
                }
            }
        } else if (mqtt_topic_equals(event, ESP32_DOWNLOAD_TOPIC)) {
            if (!model_download_armed) {
                publish_ack("update_model_ignored");
                ESP_LOGW(TAG, "Ignoring update_model until training_complete is received");
            } else {
                publish_ack("update_model");
                if (model_download_in_progress) {
                    ESP_LOGW(TAG, "Ignoring update_model: model download already in progress");
                } else {
                    ESP_LOGI(TAG, "Model download requested (update_model)");
                    model_download_in_progress = true;
                    if (xTaskCreate(download_model_and_params_task,
                                    "model_dl",
                                    8192,
                                    NULL,
                                    5,
                                    NULL) != pdPASS) {
                        model_download_in_progress = false;
                        ESP_LOGE(TAG, "Failed to create model download task");
                    }
                }
            }
        } else if (mqtt_topic_equals(event, ESP32_LOAD_MODEL_TOPIC)) {
            // Manual model load for debugging: bypass training_complete requirement
            publish_ack("load_model");
            if (model_download_in_progress) {
                ESP_LOGW(TAG, "Ignoring load_model: model download already in progress");
            } else {
                ESP_LOGI(TAG, "Manual model load requested (load_model)");
                model_download_in_progress = true;
                if (xTaskCreate(download_model_and_params_task,
                                "model_dl",
                                8192,
                                NULL,
                                5,
                                NULL) != pdPASS) {
                    model_download_in_progress = false;
                    ESP_LOGE(TAG, "Failed to create model download task");
                }
            }
        }
        break;

    default:
        break;
    }
}

// WiFi event handler moved to file scope
static EventGroupHandle_t wifi_event_group;
static uint8_t s_retry_num = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START received, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED received");
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP (%d/5)", s_retry_num);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP received, IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connected = true;

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGW("CSI", "Target BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                     ap_info.bssid[0],
                     ap_info.bssid[1],
                     ap_info.bssid[2],
                     ap_info.bssid[3],
                     ap_info.bssid[4],
                     ap_info.bssid[5]);
            ESP_LOGI("CSI", "Primary Channel: %d", ap_info.primary);
            memcpy(dynamic_target_mac, ap_info.bssid, 6);
        }
    }
}

static void vLogFreeHeap(void *pvParameters)
{
    (void)pvParameters;

    // Ensure first heartbeat is sent even if no state change occurs.
    last_published_mqtt_connected = !mqtt_connected;
    last_published_model_ready = !model_ready;
    last_published_model_download_armed = !model_download_armed;

    for (;;) {
        // Wake on state changes (notify_status_task) or after a long interval.
        // Using a long timeout reduces wakeups and keeps CPU usage low.
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

        bool changed = (mqtt_connected != last_published_mqtt_connected) ||
                       (model_ready != last_published_model_ready) ||
                       (model_download_armed != last_published_model_download_armed);

        // Ping at least once every HEARTBEAT_INTERVAL_MS to keep dashboard aware.
        if (!changed && notified == 0) {
            changed = true;
        }

        if (!changed) {
            continue;
        }

        last_published_mqtt_connected = mqtt_connected;
        last_published_model_ready = model_ready;
        last_published_model_download_armed = model_download_armed;

        uint32_t heap = esp_get_free_heap_size();
        ESP_LOGI("FREE_HEAP", "Free Heap: %" PRIu32 " bytes | MQTT=%s | model_ready=%d | download_armed=%d",
                 heap,
                 mqtt_connected ? "yes" : "no",
                 model_ready,
                 model_download_armed);

        char hb_buf[128];
        int len = snprintf(hb_buf, sizeof(hb_buf),
                           "{\"event\":\"heartbeat\",\"mqtt_connected\":%s,\"model_ready\":%s,\"model_download_armed\":%s}",
                           mqtt_connected ? "true" : "false",
                           model_ready ? "true" : "false",
                           model_download_armed ? "true" : "false");
        if (len > 0 && len < (int)sizeof(hb_buf)) {
            if (mqtt_connected) {
                if (!publish_with_retry(ESP32_STATUS_TOPIC, hb_buf, 1)) {
                    ESP_LOGW(TAG, "Heartbeat publish failed");
                }
            } else {
                ESP_LOGI(TAG, "Skipping heartbeat publish: MQTT disconnected");
            }
        } else {
            ESP_LOGW(TAG, "Heartbeat message truncated");
        }
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wifi_config = {};
    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASS, strlen(WIFI_PASS));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.listen_interval = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "WiFi connection TIMEOUT or UNKNOWN EVENT");
    }
    vEventGroupDelete(wifi_event_group);

    esp_wifi_set_promiscuous(true);

    wifi_promiscuous_filter_t m_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL,
    };
    esp_wifi_set_promiscuous_filter(&m_filter);

    wifi_promiscuous_filter_t c_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL,
    };
    esp_wifi_set_promiscuous_ctrl_filter(&c_filter);

    wifi_csi_config_t csi_cfg = {};
    csi_cfg.lltf_en = true;
    csi_cfg.htltf_en = false;
    csi_cfg.stbc_htltf2_en = false;
    csi_cfg.ltf_merge_en = true;
    csi_cfg.channel_filter_en = true;
    csi_cfg.manu_scale = false;
    esp_wifi_set_csi_config(&csi_cfg);
    esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL);
    esp_wifi_set_csi(true);

    ESP_LOGI(TAG, "WiFi init and CSI setup complete");
    xTaskCreate(vLogFreeHeap, "LogFreeHeap", 2048, NULL, 5, &status_task_handle);

    // Inference offload: do TFLM inference outside of the Wi‑Fi CSI callback
    // so the Wi‑Fi stack and MQTT keep-alive remain responsive.
    inference_queue = xQueueCreate(INFERENCE_QUEUE_DEPTH, sizeof(inference_sample_t));
    if (inference_queue) {
        // Run inference at a lower priority than the Wi-Fi/MQTT stack so
        // they remain responsive even when inference computation is heavy.
        xTaskCreate(vInferenceTask, "Inference", 8192, NULL, 2, NULL);
    } else {
        ESP_LOGW(TAG, "Failed to create inference queue");
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    for (int i = 0; i < MAX_MODEL_FEATURES_PER_FRAME; i++) {
        std_vals[i] = 1.0f;
    }
    model_features_per_frame = ACTIVE_SUBCARRIERS;
    model_input_elements = model_features_per_frame;
    tflm_reset();
    reset_state_machine();

    wifi_init_sta();

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_cfg.session.last_will.topic = ESP32_DEVICE_STATUS_TOPIC;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;

    esp_mqtt_client_handle_t client_handle = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client_handle,
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(client_handle);
}