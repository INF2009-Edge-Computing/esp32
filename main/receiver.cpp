extern "C" {
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "rom/ets_sys.h"
#include "cJSON.h"
}

#include "tflm_inference.h"

#define ESP32_ID RACK_1

#define STR(x) #x
#define XSTR(x) STR(x)
#define ESP32_COLLECT_TOPIC "/commands/" XSTR(ESP32_ID) "/collect"
#define ESP32_DOWNLOAD_TOPIC "/commands/" XSTR(ESP32_ID) "/update_model"
#define ESP32_TRAINING_COMPLETE_TOPIC "/commands/" XSTR(ESP32_ID) "/training_complete"
#define ESP32_STATUS_TOPIC "/sensors/" XSTR(ESP32_ID) "/status"

// Set your WiFi credentials here
#define WIFI_SSID "<WIFI_NAME>"
#define WIFI_PASS "<WIFI_PASSWORD>"

// Set your Pi 5 IP address here
#define MQTT_BROKER_URI "mqtt://192.168.1.9:1883"
#define HTTP_UPLOAD_URI "http://192.168.1.9:5000/upload_data"
#define HTTP_SERVER_BASE "http://192.168.1.9:5000"
#define HTTP_MODEL_URI HTTP_SERVER_BASE "/model/" XSTR(ESP32_ID)
#define HTTP_PARAMS_URI HTTP_SERVER_BASE "/params/" XSTR(ESP32_ID)
#define DEFAULT_SCAN_LIST_SIZE 20

/* Define range of subcarriers to use for CSI, ignoring noisy/unused ones */
#define MAX_LOWER 4
#define MAX_UPPER 60
#define DC_NULL 32
#define NUM_SUBCARRIERS 64
#define EXPECTED_PERIOD_US 100000 // 10Hz expected period between packets

#define ACTIVE_SUBCARRIERS (MAX_UPPER - MAX_LOWER)
static int64_t last_trigger_us = 0; // Last time we accepted a packet for processing

#define SAMPLE_SIZE 200                                  // 20 seconds of data at 10Hz
#define SUB_BATCH_SIZE 40                                // packets per HTTP sub-batch
static uint8_t csi_buffer[SUB_BATCH_SIZE][ACTIVE_SUBCARRIERS];
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
static float mean_vals[ACTIVE_SUBCARRIERS] = {0};
static float std_vals[ACTIVE_SUBCARRIERS] = {0};
static int pred_history[10] = {0};
static int pred_history_idx = 0;
static int pred_history_count = 0;
static int current_majority_state = -1;
static bool wifi_connected = false;

typedef struct {
    char *label;
    uint8_t sub_batch_idx;
    uint8_t total_sub_batches;
} http_task_params_t;

static esp_http_client_handle_t client = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;

static bool publish_with_retry(const char *topic, const char *payload, int qos)
{
    if (mqtt_client == NULL) {
        ESP_LOGW(TAG, "publish_with_retry: mqtt_client is NULL");
        return false;
    }

    const int max_attempts = 5;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, 0);
        if (msg_id >= 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt)));
    }

    ESP_LOGW(TAG, "publish failed topic=%s payload=%s", topic, payload);
    return false;
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

static bool mqtt_topic_equals(esp_mqtt_event_handle_t event, const char *topic)
{
    size_t expected_len = strlen(topic);
    return event->topic_len == (int)expected_len && strncmp(event->topic, topic, expected_len) == 0;
}

static void reset_prediction_history(void)
{
    memset(pred_history, 0, sizeof(pred_history));
    pred_history_idx = 0;
    pred_history_count = 0;
    current_majority_state = -1;
}

static esp_err_t http_get_buffer(const char *url, uint8_t **out_buf, int *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
    };

    esp_http_client_handle_t h = esp_http_client_init(&config);
    if (!h) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(h);
    if (err != ESP_OK) {
        esp_http_client_cleanup(h);
        return err;
    }

    int status = esp_http_client_get_status_code(h);
    if (status < 200 || status >= 300) {
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    int content_len = esp_http_client_get_content_length(h);
    int alloc_len = (content_len > 0 && content_len < (1024 * 1024)) ? (content_len + 1) : (64 * 1024);
    uint8_t *buf = (uint8_t *)malloc(alloc_len);
    if (!buf) {
        esp_http_client_cleanup(h);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read_response(h, (char *)buf, alloc_len - 1);
    if (read_len <= 0) {
        free(buf);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    buf[read_len] = '\0';
    *out_buf = buf;
    *out_len = read_len;

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

    for (int i = 0; i < ACTIVE_SUBCARRIERS; i++) {
        mean_vals[i] = 0.0f;
        std_vals[i] = 1.0f;
    }

    int mean_sz = cJSON_GetArraySize(mean_arr);
    int std_sz = cJSON_GetArraySize(std_arr);
    int lim = mean_sz < std_sz ? mean_sz : std_sz;
    if (lim > ACTIVE_SUBCARRIERS) {
        lim = ACTIVE_SUBCARRIERS;
    }

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

    cJSON_Delete(root);
    return true;
}

static int run_model(const float *input, size_t len)
{
    if (!model_ready || !input || len == 0) {
        return -1;
    }

    int pred = tflm_predict(input, len);
    if (pred < 0 || pred > 2) {
        ESP_LOGW(TAG, "tflm_predict failed: %s", tflm_last_error());
        return -1;
    }
    return pred;
}

static void update_majority_and_notify(int pred)
{
    if (pred < 0 || pred > 2) {
        return;
    }

    pred_history[pred_history_idx] = pred;
    pred_history_idx = (pred_history_idx + 1) % 10;
    if (pred_history_count < 10) {
        pred_history_count++;
    }

    if (pred_history_count < 10) {
        return;
    }

    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 10; i++) {
        int v = pred_history[i];
        if (v >= 0 && v <= 2) {
            counts[v]++;
        }
    }

    int maj = 0;
    if (counts[1] > counts[maj]) {
        maj = 1;
    }
    if (counts[2] > counts[maj]) {
        maj = 2;
    }

    if (maj == current_majority_state) {
        return;
    }

    // IMPORTANT: align with train_model.py label mapping.
    static const char *states[3] = {"door_open", "door_closed", "person_standing"};

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", "state_change");
    cJSON_AddStringToObject(obj, "state", states[maj]);

    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);

    current_majority_state = maj;
    ESP_LOGI(TAG, "state_change => %s", states[maj]);
}

static void download_model_and_params_task(void *pvParameters)
{
    (void)pvParameters;

    for (int attempt = 1; attempt <= 3; attempt++) {
        uint8_t *downloaded_model = NULL;
        uint8_t *downloaded_params = NULL;
        int model_len = 0;
        int params_len = 0;

        esp_err_t e1 = http_get_buffer(HTTP_MODEL_URI, &downloaded_model, &model_len);
        esp_err_t e2 = http_get_buffer(HTTP_PARAMS_URI, &downloaded_params, &params_len);

        if (e1 == ESP_OK && e2 == ESP_OK) {
            bool scaler_ok = load_scaler_params(downloaded_params, params_len);
            bool model_ok = tflm_load_model(downloaded_model, (size_t)model_len);
            if (scaler_ok && model_ok) {
                if (model_buffer) {
                    free(model_buffer);
                }
                model_buffer = downloaded_model;
                model_size = (size_t)model_len;
                model_ready = true;
                reset_prediction_history();

                cJSON *obj = cJSON_CreateObject();
                if (obj) {
                    cJSON_AddStringToObject(obj, "event", "model_ready");
                    cJSON_AddNumberToObject(obj, "bytes", (double)model_size);
                    char *json_str = cJSON_PrintUnformatted(obj);
                    if (json_str) {
                        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
                        free(json_str);
                    }
                    cJSON_Delete(obj);
                }

                free(downloaded_params);
                ESP_LOGI(TAG, "Model/scaler downloaded (%d bytes)", model_len);
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
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    model_ready = false;
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

    vTaskDelete(NULL);
}

static void send_batch_http_task(void *pvParameters)
{
    if (pvParameters == NULL || mqtt_client == NULL) {
        ESP_LOGE(TAG, "HTTP task received NULL parameters or uninitialized MQTT client");
        vTaskDelete(NULL);
        return;
    }

    http_task_params_t *params = (http_task_params_t *)pvParameters;

    esp_http_client_config_t config = {
        .url = HTTP_UPLOAD_URI,
        .method = HTTP_METHOD_POST,
    };

    // Initialize HTTP client on first use, then reuse for subsequent batches
    if (client == NULL) {
        client = esp_http_client_init(&config);
    }

    // Tell server the label for this batch in HTTP headers
    esp_http_client_set_header(client, "X-Room-State", params->label);
    esp_http_client_set_header(client, "X-ESP32-ID", XSTR(ESP32_ID));
    if (current_session_id[0] != '\0') {
        esp_http_client_set_header(client, "X-Session-ID", current_session_id);
    }

    // Progress tracking headers for server-side assembly of batches
    char sub_batch_str[6];
    snprintf(sub_batch_str, sizeof(sub_batch_str), "%d", params->sub_batch_idx);
    esp_http_client_set_header(client, "X-Sub-Batch-Index", sub_batch_str);

    char total_batches_str[6];
    snprintf(total_batches_str, sizeof(total_batches_str), "%d", params->total_sub_batches);
    esp_http_client_set_header(client, "X-Total-Sub-Batches", total_batches_str);

    // Set header to tell server this is binary data
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(client, (const char *)csi_buffer, sizeof(csi_buffer));

    uint32_t heap_before = esp_get_free_heap_size();
    uint32_t min_heap_before = esp_get_minimum_free_heap_size();

    esp_err_t err = esp_http_client_perform(client);

    uint32_t heap_after = esp_get_free_heap_size();
    uint32_t min_heap_after = esp_get_minimum_free_heap_size();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Batch uploaded successfully (%d/%d)", params->sub_batch_idx + 1, params->total_sub_batches);
        ESP_LOGW("MEM_STATS", "HTTP call stats: used %lu bytes (peak %lu during call)",
                 (unsigned long)(heap_before - heap_after),
                 (unsigned long)(min_heap_before - min_heap_after));
        char progress[128];
        snprintf(progress, sizeof(progress),
                 "{\"event\":\"upload\",\"sub\":%d,\"total\":%d}",
                 params->sub_batch_idx + 1, params->total_sub_batches);
        publish_with_retry(ESP32_STATUS_TOPIC, progress, 1);
    } else {
        ESP_LOGE(TAG, "HTTP upload failed (err=%d) for sub-batch %d", err, params->sub_batch_idx + 1);
        char failmsg[128];
        snprintf(failmsg, sizeof(failmsg),
                 "{\"event\":\"upload_failed\",\"sub\":%d,\"err\":%d}",
                 params->sub_batch_idx + 1, err);
        publish_with_retry(ESP32_STATUS_TOPIC, failmsg, 1);

        int retries = 0;
        while (retries < 3 && err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500 * (retries + 1)));
            err = esp_http_client_perform(client);
            retries++;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Final upload failure after retries, aborting collection");
            is_collecting = false;
        }
    }

    // Firmware fallback completion event after final sub-batch upload
    if (params->sub_batch_idx == params->total_sub_batches - 1) {
        esp_http_client_cleanup(client);
        client = NULL;

        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj) {
            cJSON_AddStringToObject(msg_obj, "event", "collection_complete");
            cJSON_AddStringToObject(msg_obj, "label", params->label);
            if (current_session_id[0] != '\0') {
                cJSON_AddStringToObject(msg_obj, "session", current_session_id);
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

    free(params->label);
    free(params);

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
    for (uint8_t i = 0; i < valid_sc_count; i++) {
        uint32_t normalized = (current_pwrs[i] * multiplier) >> 8;
        normalized_row[i] = (uint8_t)(normalized > 255 ? 255 : normalized);
    }

    // --- PATH A: INFERENCE (always active after model download) ---
    if (model_ready) {
        float model_input[ACTIVE_SUBCARRIERS] = {0};
        for (uint8_t i = 0; i < valid_sc_count && i < ACTIVE_SUBCARRIERS; i++) {
            float sigma = std_vals[i];
            if (sigma == 0.0f) {
                sigma = 1.0f;
            }
            model_input[i] = (((float)normalized_row[i]) - mean_vals[i]) / sigma;
        }
        int pred = run_model(model_input, ACTIVE_SUBCARRIERS);
        update_majority_and_notify(pred);
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
                params->label = strdup(collection_label);
                params->sub_batch_idx = sub_batch_idx;
                params->total_sub_batches = (SAMPLE_SIZE / SUB_BATCH_SIZE);

                if (xTaskCreate(send_batch_http_task,
                                "http_batch_upload",
                                8192,
                                params,
                                5,
                                NULL) != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create HTTP upload task");
                    free(params->label);
                    free(params);
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
        ESP_LOGI(TAG, "MQTT Connected");
        esp_mqtt_client_subscribe(event->client, ESP32_COLLECT_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, ESP32_DOWNLOAD_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, ESP32_TRAINING_COMPLETE_TOPIC, 1);
        mqtt_client = event->client;
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received data on topic: %.*s", event->topic_len, event->topic);

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
            reset_prediction_history();

            ESP_LOGI(TAG, "Starting data collection for state: %s (session=%s)",
                     collection_label,
                     current_session_id);
        } else if (mqtt_topic_equals(event, ESP32_TRAINING_COMPLETE_TOPIC)) {
            publish_ack("training_complete");
            ESP_LOGI(TAG, "Model download requested (training_complete)");
            model_download_armed = true;

            if (xTaskCreate(download_model_and_params_task,
                            "model_dl",
                            8192,
                            NULL,
                            5,
                            NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to create model download task");
            }
        } else if (mqtt_topic_equals(event, ESP32_DOWNLOAD_TOPIC)) {
            if (!model_download_armed) {
                publish_ack("update_model_ignored");
                ESP_LOGW(TAG, "Ignoring update_model until training_complete is received");
            } else {
                publish_ack("update_model");
                ESP_LOGI(TAG, "Model download requested (update_model)");
                if (xTaskCreate(download_model_and_params_task,
                                "model_dl",
                                8192,
                                NULL,
                                5,
                                NULL) != pdPASS) {
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

    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(10000);
    xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        ESP_LOGI("FREE_HEAP", "Free Heap: %" PRIu32 " bytes", esp_get_free_heap_size());
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

    wifi_csi_config_t csi_cfg = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
    };
    esp_wifi_set_csi_config(&csi_cfg);
    esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL);
    esp_wifi_set_csi(true);

    ESP_LOGI(TAG, "WiFi init and CSI setup complete");
    xTaskCreate(vLogFreeHeap, "LogFreeHeap", 2048, NULL, 5, NULL);
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    for (int i = 0; i < ACTIVE_SUBCARRIERS; i++) {
        std_vals[i] = 1.0f;
    }
    tflm_reset();

    wifi_init_sta();

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;

    esp_mqtt_client_handle_t client_handle = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client_handle,
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(client_handle);
}