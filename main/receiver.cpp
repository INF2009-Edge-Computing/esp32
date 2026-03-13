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

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    esp_mqtt_client_handle_t client_handle = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client_handle,
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(client_handle);
}extern "C" {
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "regex.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "rom/ets_sys.h"
<<<<<<< HEAD:main/receiver.c
#include "cJSON.h"  // JSON parsing helper
=======
#include "mdns.h"
#include "cJSON.h"
}

// TFLite Includes
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/system_setup.h"
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp

#define ESP32_ID RACK_1

#define STR(x) #x
#define XSTR(x) STR(x)
#define ESP32_COLLECT_TOPIC "/commands/" XSTR(ESP32_ID) "/collect"
#define ESP32_DOWNLOAD_TOPIC "/commands/" XSTR(ESP32_ID) "/update_model"
#define ESP32_ASSIGN_TOPIC "/commands/" XSTR(ESP32_ID) "/assign_name"
#define ESP32_STATUS_TOPIC "/sensors/" XSTR(ESP32_ID) "/status"

// Set your WiFi credentials here
<<<<<<< HEAD:main/receiver.c
#define WIFI_SSID "YourSSID"
#define WIFI_PASS  "password123"

// Set your Pi 5 IP address here
#define MQTT_BROKER_URI "mqtt://10.198.7.22:1883"
#define HTTP_UPLOAD_URI "http://10.198.7.22:5000/upload_data"
#define HTTP_SERVER_BASE "http://10.198.7.22:5000"
#define HTTP_MODEL_URI HTTP_SERVER_BASE "/model/" XSTR(ESP32_ID)
#define HTTP_PARAMS_URI HTTP_SERVER_BASE "/params/" XSTR(ESP32_ID)
=======
#define WIFI_SSID "<WIFI_SSID>"
#define WIFI_PASS "<WIFI_PASS>"

// Set your Pi 5 IP address here
#define MQTT_BROKER_URI "mqtt://192.168.137.106:1883"
#define HTTP_UPLOAD_URI "http://192.168.137.106:5000/upload_data"
#define HTTP_DOWNLOAD_URI "http://192.168.137.106:5000/static/models/" XSTR(ESP32_ID)
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp
#define DEFAULT_SCAN_LIST_SIZE 20

/* Define range of subcarriers to use for CSI, ignoring noisy/unused ones */
#define MAX_LOWER 4
#define MAX_UPPER 60
#define DC_NULL 32
#define NUM_SUBCARRIERS 64
#define EXPECTED_PERIOD_US 100000 // 100ms gate (10Hz)

// number of subcarriers we actually process (upper-lower+1 minus the DC)
#define ACTIVE_SUBCARRIERS (MAX_UPPER - MAX_LOWER)
static int64_t last_trigger_us = 0; //Last time we accepted a packet for processing

#define SAMPLE_SIZE 200                                 // 20 seconds of data at 10Hz
#define SUB_BATCH_SIZE 40                               // How many packets of data store before sending to the server (to avoid large memory spikes)
uint8_t csi_buffer[SUB_BATCH_SIZE][ACTIVE_SUBCARRIERS]; // Store only amplitudes
u_int8_t packet_idx = 0;
u_int8_t sub_batch_idx = 0;

// TFLite Global Variables
namespace {
    const tflite::Model* model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    TfLiteTensor* input = nullptr;
    TfLiteTensor* output = nullptr;

    const int kTensorArenaSize = 10 * 1024; // Adjust this based on your model size
    uint8_t tensor_arena[kTensorArenaSize];

    // Dynamic model storage
    uint8_t* dynamic_model_buffer = nullptr;
    size_t dynamic_model_size = 0;
    bool model_update_pending = false;
}

void tflite_init(); // Forward declaration

void download_model_task(void *pvParameters) {
    char* url = (char*)pvParameters;
    ESP_LOGI("HTTP_MODEL", "Downloading model from: %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE("HTTP_MODEL", "Failed to init client");
        free(url);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE("HTTP_MODEL", "Failed to open connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE("HTTP_MODEL", "Invalid content length: %d", content_length);
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    // Allocate RAM for the model
    uint8_t* buffer = (uint8_t*)malloc(content_length);
    if (!buffer) {
        ESP_LOGE("HTTP_MODEL", "Could not allocate %d bytes for model", content_length);
        esp_http_client_cleanup(client);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    int read_bytes = esp_http_client_read_response(client, (char*)buffer, content_length);
    if (read_bytes != content_length) {
        ESP_LOGE("HTTP_MODEL", "Read mismatch: %d != %d", read_bytes, content_length);
        free(buffer);
    } else {
        ESP_LOGI("HTTP_MODEL", "Successfully downloaded %d bytes", read_bytes);
        
        // Atomic swap or flag
        uint8_t* old_buffer = dynamic_model_buffer;
        dynamic_model_buffer = buffer;
        dynamic_model_size = (size_t)read_bytes;
        model_update_pending = true;

        if (old_buffer) {
            ESP_LOGI("HTTP_MODEL", "Freeing old model buffer");
            // Note: In a real system, ensure nothing is using old_buffer before freeing
            // Since we'll recreate the interpreter, it's safer to wait a bit
            vTaskDelay(pdMS_TO_TICKS(100));
            free(old_buffer);
        }
    }

    esp_http_client_cleanup(client);
    free(url);
    vTaskDelete(NULL);
}

void tflite_init() {
    ESP_LOGI("TFLITE", "Initializing TFLite...");
    tflite::InitializeTarget();

    if (dynamic_model_buffer == nullptr) {
        ESP_LOGW("TFLITE", "No model downloaded yet. Waiting for MQTT trigger...");
        return;
    }
    
    model = tflite::GetModel(dynamic_model_buffer);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE("TFLITE", "Model version mismatch!");
        return;
    }

    static tflite::MicroMutableOpResolver<5> resolver;
    static bool ops_added = false;
    if (!ops_added) {
        resolver.AddFullyConnected();
        resolver.AddSoftmax();
        resolver.AddRelu(); 
        ops_added = true;
    }

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);
    
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE("TFLITE", "AllocateTensors() failed");
        return;
    }

    input = interpreter->input(0);
    output = interpreter->output(0);
    ESP_LOGI("TFLITE", "TFLite Ready (Dynamic Model Loaded)");
}

void tflite_task(void *pvParameters) {
    tflite_init();
    while (1) {
        if (model_update_pending) {
            ESP_LOGI("TFLITE", "New model detected! Re-initializing...");
            model_update_pending = false;
            tflite_init();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void run_inference(uint8_t* csi_row, size_t size) {
    if (interpreter == nullptr || input == nullptr) return;

    // 1. Fill 'input' buffer with your CSI data (converting uint8_t to float)
    for (int i = 0; i < size && i < input->dims->data[1]; i++) {
        input->data.f[i] = (float)csi_row[i] / 100.0f; 
    }

    // 2. Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE("TFLITE", "Invoke failed!");
        return;
    }

    // 3. Read 'output' results (3 classes: open, close, person)
    float* results = output->data.f;
    int current_pred = 0;
    float max_val = results[0];
    for (int i = 1; i < 3; i++) {
        if (results[i] > max_val) {
            max_val = results[i];
            current_pred = i;
        }
    }

    // --- STEADY STATE DETECTION ---
    static int last_stable_pred = -1;
    static int consecutive_count = 0;
    const int STEADY_THRESHOLD = 5; // Log after 5 consecutive identical high-confidence packets

    if (max_val > 0.4f) { // High confidence required
        if (current_pred == last_stable_pred) {
            consecutive_count++;
        } else {
            last_stable_pred = current_pred;
            consecutive_count = 1;
        }

        if (consecutive_count == STEADY_THRESHOLD) {
            const char* labels[] = {"OPEN", "CLOSE", "PERSON"};
            ESP_LOGW("STEADY_STATE", "Confirmed State: %s (Stability: %d pkts)", 
                     labels[current_pred], STEADY_THRESHOLD);
            consecutive_count = 0; // Reset after logging to avoid repeated spamming
        }
    } else {
        consecutive_count = 0;
    }
}
static bool is_collecting = false;
static char collection_label[12] = "unknown";
static uint8_t dynamic_target_mac[6];
static bool is_mac_locked = false;

// track current calibration session (sent by dashboard)
static char current_session_id[32] = "";

static const char *TAG = "logger";
static esp_mqtt_client_handle_t global_client = NULL;
// client handle that will be filled on connect and used by helper functions
static esp_mqtt_client_handle_t mqtt_client = NULL;
float baseline_amp[NUM_SUBCARRIERS] = {0};
bool wifi_connected = false;

static uint8_t *model_buffer = NULL;
static size_t model_size = 0;
static bool model_ready = false;
static float mean_vals[ACTIVE_SUBCARRIERS] = {0};
static float std_vals[ACTIVE_SUBCARRIERS] = {0};
static int pred_history[10] = {0};
static int pred_history_idx = 0;
static int pred_history_count = 0;
static int current_majority_state = -1;

// forward declaration
static bool publish_with_retry(const char *topic, const char *payload, int qos);

static void publish_ack(const char *cmd)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "event", "ack");
    cJSON_AddStringToObject(obj, "cmd", cmd);
    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
        free(json_str);
    }
    cJSON_Delete(obj);
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
        return false;
    }
    cJSON *mean_arr = cJSON_GetObjectItem(root, "mean");
    cJSON *std_arr = cJSON_GetObjectItem(root, "std");
    if (!cJSON_IsArray(mean_arr) || !cJSON_IsArray(std_arr)) {
        cJSON_Delete(root);
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

    // Placeholder inference until TFLM is integrated.
    float avg = 0.0f;
    for (size_t i = 0; i < len; i++) {
        avg += input[i];
    }
    avg /= (float)len;

    if (avg < -0.3f) {
        return 0; // door_closed
    }
    if (avg > 0.7f) {
        return 1; // door_open
    }
    return 2; // person_standing
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
        return; // wait until the 10-sample window is full
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

    const char *states[3] = {"door_closed", "door_open", "person_standing"};
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

        if (e1 == ESP_OK && e2 == ESP_OK && load_scaler_params(downloaded_params, params_len)) {
            if (model_buffer) {
                free(model_buffer);
            }
            model_buffer = downloaded_model;
            model_size = (size_t)model_len;
            model_ready = true;
            free(downloaded_params);

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

            ESP_LOGI(TAG, "Model/scaler download successful, bytes=%d", model_len);
            vTaskDelete(NULL);
            return;
        }

        if (downloaded_model) {
            free(downloaded_model);
        }
        if (downloaded_params) {
            free(downloaded_params);
        }

        ESP_LOGW(TAG, "Model/scaler download attempt %d/3 failed", attempt);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

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

// helper to publish with a few retries & exponential backoff
static bool publish_with_retry(const char *topic, const char *payload, int qos)
{
    const int max_attempts = 5;
    int attempt = 0;
    while (attempt < max_attempts) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, 0);
        if (msg_id >= 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt))); // 100ms,200ms,400ms...
        attempt++;
    }
    ESP_LOGW(TAG, "publish_with_retry failed topic=%s payload=%s", topic, payload);
    return false;
}

<<<<<<< HEAD:main/receiver.c

=======
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp
typedef struct {
    char *label;
    u_int8_t sub_batch_idx;
    u_int8_t total_sub_batches;
} http_task_params_t;

esp_http_client_handle_t client;
void send_batch_http_task(void *pvParameters)
{
    if(pvParameters == NULL || mqtt_client == NULL) {
        ESP_LOGE(TAG, "HTTP Task received NULL parameters or uninitialized MQTT client");
        vTaskDelete(NULL);
        return;
    }
    
    http_task_params_t *params = (http_task_params_t *)pvParameters;
    
    esp_http_client_config_t config = {
        .url = HTTP_UPLOAD_URI,
        .method = HTTP_METHOD_POST,
    };
    
    // Initialize HTTP client on first use, then reuse for subsequent batches
    if(client == NULL) {
        client = esp_http_client_init(&config);
    }

    // Tell server the label for this batch in HTTP headers (e.g., "empty" or "occupied")
    esp_http_client_set_header(client, "X-Room-State", params->label);
    // Also include ESP32 ID for server-side identification of data source
    esp_http_client_set_header(client, "X-ESP32-ID", XSTR(ESP32_ID));
    // propagate session identifier so server can isolate concurrent runs
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

    // Debug free heap to track memory usage of HTTP upload
    uint32_t heap_before = esp_get_free_heap_size();
    uint32_t min_heap_before = esp_get_minimum_free_heap_size();

    esp_err_t err = esp_http_client_perform(client);

    uint32_t heap_after = esp_get_free_heap_size();
    uint32_t min_heap_after = esp_get_minimum_free_heap_size();
    
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Batch uploaded successfully (%d/%d)", params->sub_batch_idx + 1, params->total_sub_batches);
        ESP_LOGW("MEM_STATS", "HTTP Call Stats: Used %lu bytes (Peak used %lu bytes during call)", 
                 (unsigned long)(heap_before - heap_after), (unsigned long)(min_heap_before - min_heap_after));
        // send progress event
        char progress[128];
        snprintf(progress, sizeof(progress), "{\"event\":\"upload\",\"sub\":%d,\"total\":%d}",
                 params->sub_batch_idx + 1, params->total_sub_batches);
        publish_with_retry(ESP32_STATUS_TOPIC, progress, 1);
    } else {
        ESP_LOGE(TAG, "HTTP upload failed (err=%d) for sub-batch %d", err, params->sub_batch_idx + 1);
        char failmsg[128];
        snprintf(failmsg, sizeof(failmsg), "{\"event\":\"upload_failed\",\"sub\":%d,\"err\":%d}",
                 params->sub_batch_idx + 1, err);
        publish_with_retry(ESP32_STATUS_TOPIC, failmsg, 1);
        // simple retry strategy
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
    
    // If this was the last sub-batch, clean up the HTTP client to free memory and notify server
    if (params->sub_batch_idx == params->total_sub_batches - 1) {
        esp_http_client_cleanup(client);
        client = NULL;

        // build JSON safely so the label string is properly escaped
        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj) {
            cJSON_AddStringToObject(msg_obj, "event", "collection_complete");
            cJSON_AddStringToObject(msg_obj, "label", params->label);
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
    if(wifi_connected == false || info == NULL)
        return;

#ifndef CSI_DEBUG
    // During calibration, accept all real packets on-channel to increase
    // sampling frequency without generating synthetic traffic.
    if (!is_collecting && memcmp(info->mac, dynamic_target_mac, 6) != 0)
        return;  // idle mode: keep only router packets
#endif
    
    int64_t now = esp_timer_get_time();
    int64_t dt = now - last_trigger_us;

    // Only accept one packet per MIN_INTERVAL_US 
    if (dt < EXPECTED_PERIOD_US) {
        // ESP_LOGD(TAG, "Skipping duplicate packet (dt=%lld)", dt);
        return;
    }

    // Filter out outlier packets for consistent CSI data
#ifndef CSI_DEBUG
    // relax RSSI threshold to -100 so weak packets are kept
    if (info->rx_ctrl.sig_len > 32 || info->rx_ctrl.rssi < -100 || info->rx_ctrl.rx_state != 0) 
        return;
#endif

    last_trigger_us = now; // update AFTER deciding to accept

    // Extract raw data
    int8_t *raw = (int8_t *)info->buf;
    uint16_t current_pwrs[MAX_UPPER - MAX_LOWER + 1];
    uint32_t sum_pwr = 0;
    u_int8_t valid_sc_count = 0;

    // Normalization Pass 1: Calculate raw amplitudes & packet average
    for (u_int8_t sc = MAX_LOWER; sc <= MAX_UPPER; sc++)
    {
        if (sc == DC_NULL)
            continue;

        int8_t i = raw[sc * 2];
        int8_t q = raw[sc * 2 + 1];

        // Squared amplitude (power) of the subcarrier
        uint16_t pwr = (uint16_t)(i * i + q * q);

        current_pwrs[sc - MAX_LOWER] = pwr;
        sum_pwr += pwr;
        valid_sc_count++;
    }

    if (valid_sc_count == 0) {
        ESP_LOGI(TAG, "No valid subcarriers found in this packet"); 
        return;
    }
           
    float avg_amp = sum_pwr / valid_sc_count;
#ifdef CSI_DEBUG
    ESP_LOGD(TAG, "valid_sc_count=%d avg_amp=%.2f rssi=%d sig_len=%d", valid_sc_count, avg_amp, info->rx_ctrl.rssi, info->rx_ctrl.sig_len);
#endif

    // Normalization Pass 2: Create the 'Feature Vector'
    uint8_t normalized_row[ACTIVE_SUBCARRIERS];
    // Fixed-point normalization:
    // Result = (pwr * 100) / avg
    // To avoid division in a loop, calculate a "multiplier"
    if (sum_pwr == 0)
        return;
    uint32_t multiplier = (100 * valid_sc_count << 8) / sum_pwr;

    for (u_int8_t i = 0; i < valid_sc_count; i++)
    {
        // Use shift-right to simulate decimal division
        uint32_t normalized = (current_pwrs[i] * multiplier) >> 8;
        normalized_row[i] = (uint8_t)(normalized > 255 ? 255 : normalized);
    }

    // log normalized row for debugging
    // ESP_LOGI(TAG, "CSI Packet: Avg Amp=%.2f", avg_amp);

    // --- PATH A: INFERENCE (Always Active) ---
<<<<<<< HEAD:main/receiver.c
    float model_input[ACTIVE_SUBCARRIERS] = {0};
    for (u_int8_t i = 0; i < valid_sc_count && i < ACTIVE_SUBCARRIERS; i++)
    {
        float sigma = std_vals[i];
        if (sigma == 0.0f) {
            sigma = 1.0f;
        }
        model_input[i] = (((float)normalized_row[i]) - mean_vals[i]) / sigma;
    }
    int pred = run_model(model_input, valid_sc_count);
    update_majority_and_notify(pred);
=======
    run_inference(normalized_row, sizeof(normalized_row));
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp

    // --- PATH B: DATA COLLECTION (Gated by MQTT) ---
    if (is_collecting)
    {
        // Copy the normalized row into the global BATCH buffer
        if (packet_idx < SUB_BATCH_SIZE)
        {
            memcpy(csi_buffer[packet_idx], normalized_row, sizeof(normalized_row));
            packet_idx++;
        }

        // Check if the batch is complete
        if (packet_idx >= SUB_BATCH_SIZE) // Trigger upload at sub-batch size to avoid large memory spikes
        {
            ESP_LOGI(TAG, "Sub-batch full (%d/%d). Triggering upload for state: %s", sub_batch_idx + 1, (SAMPLE_SIZE / SUB_BATCH_SIZE), collection_label);

            http_task_params_t *params = (http_task_params_t *)malloc(sizeof(http_task_params_t));
            if (params != NULL) {
                params->label = strdup(collection_label);
                params->sub_batch_idx = sub_batch_idx;
                params->total_sub_batches = (SAMPLE_SIZE / SUB_BATCH_SIZE);

                // Create HTTP Task.
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

            packet_idx = 0;  // Reset for the next MQTT trigger
            sub_batch_idx++; // Increment sub-batch index
            
            if(sub_batch_idx >= (SAMPLE_SIZE / SUB_BATCH_SIZE)) {
                sub_batch_idx = 0; // Reset sub-batch index after reaching max batches
                is_collecting = false; // Stop collecting until next MQTT trigger
                ESP_LOGI(TAG, "Collection complete for state: %s", collection_label);
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        // Subscribe to multiple topics
        esp_mqtt_client_subscribe(event->client, ESP32_COLLECT_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, ESP32_DOWNLOAD_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, ESP32_ASSIGN_TOPIC, 0);
        mqtt_client = event->client; // Store the client handle for use in callbacks
            cJSON_Delete(outer);
        }
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received data on topic: %.*s", event->topic_len, event->topic);

        if (strncmp(event->topic, ESP32_COLLECT_TOPIC, event->topic_len) == 0)
        {
            publish_ack("start_calibration");
            // payload may be JSON {"label":"...","session":"..."} or plain string
            char buf[256];
            snprintf(buf, sizeof(buf), "%.*s", event->data_len, event->data);
            collection_label[0] = '\0';
            current_session_id[0] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root && cJSON_IsObject(root)) {
                cJSON *jlabel = cJSON_GetObjectItem(root, "label");
                if (cJSON_IsString(jlabel)) {
                    snprintf(collection_label, sizeof(collection_label), "%s", jlabel->valuestring);
                }
                cJSON *jsess = cJSON_GetObjectItem(root, "session");
                if (cJSON_IsString(jsess)) {
                    snprintf(current_session_id, sizeof(current_session_id), "%s", jsess->valuestring);
                }
            }
            cJSON_Delete(root);
            if (!collection_label[0]) {
                // fallback to raw payload
                snprintf(collection_label, sizeof(collection_label), "%.*s", event->data_len, event->data);
            }
            if (!current_session_id[0]) {
                // generate simple timestamp-based session id
                int64_t t = esp_timer_get_time();
                snprintf(current_session_id, sizeof(current_session_id), "%lld", t);
            }

            packet_idx = 0;       // Reset buffer position
            sub_batch_idx = 0;   // Reset sub-batch position
            is_collecting = true; // Start the data collection engine
            ESP_LOGI(TAG, "Starting data collection for state: %s (session=%s)", collection_label, current_session_id);
        } else if (strncmp(event->topic, ESP32_DOWNLOAD_TOPIC, event->topic_len) == 0)
        {
<<<<<<< HEAD:main/receiver.c
            publish_ack("update_model");
            ESP_LOGI(TAG, "Model download requested!");
            if (xTaskCreate(download_model_and_params_task,
                            "model_dl",
                            8192,
                            NULL,
                            5,
                            NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to start model download task");
            }
        } else if (strncmp(event->topic, ESP32_ASSIGN_TOPIC, event->topic_len) == 0)
        {
            publish_ack("assign_name");
            char payload_buf[128];
            snprintf(payload_buf, sizeof(payload_buf), "%.*s", event->data_len, event->data);

            cJSON *root = cJSON_Parse(payload_buf);
            const char *new_name = payload_buf;
            if (root && cJSON_IsObject(root)) {
                cJSON *jname = cJSON_GetObjectItem(root, "name");
                if (cJSON_IsString(jname) && jname->valuestring && jname->valuestring[0]) {
                    new_name = jname->valuestring;
                }
            }

            nvs_handle_t nvs_h;
            if (nvs_open("device_cfg", NVS_READWRITE, &nvs_h) == ESP_OK) {
                nvs_set_str(nvs_h, "name", new_name);
                nvs_commit(nvs_h);
                nvs_close(nvs_h);
            }

            cJSON *obj = cJSON_CreateObject();
            if (obj) {
                cJSON_AddStringToObject(obj, "event", "identify_confirmed");
                cJSON_AddStringToObject(obj, "name", new_name);
                char *json_str = cJSON_PrintUnformatted(obj);
                if (json_str) {
                    publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
                    free(json_str);
                }
                cJSON_Delete(obj);
            }
            cJSON_Delete(root);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
=======
            // The download URL is now hardcoded. Just strdup the macro for the task.
            char* download_url = strdup(HTTP_DOWNLOAD_URI);
            if (download_url) {
                ESP_LOGI(TAG, "Starting model download from: %s", download_url);
                xTaskCreate(download_model_task, "download_model", 8192, download_url, 5, NULL);
            } else {
                ESP_LOGE(TAG, "Failed to strdup URL for download");
            }
        }
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp
        break;
    default:
        break;
    }
}

// WiFi event handler moved to file scope
static EventGroupHandle_t wifi_event_group;
static u_int8_t s_retry_num = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data) {
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
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP received, IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connected = true;

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            // This is the "Real MAC" of the 2.4GHz radio
            ESP_LOGW("CSI", "Target BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                     ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                     ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);

            // Confirm you are on the expected channel
            ESP_LOGI("CSI", "Primary Channel: %d", ap_info.primary);
            
            // Set the global target MAC to the AP's for CSI filtering
            memcpy(dynamic_target_mac, ap_info.bssid, 6);
            is_mac_locked = true;
        }
    }
}

void vLogFreeHeap(void *pvParameters)
{
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = pdMS_TO_TICKS(10000); // Set interval to 10 seconds

    // Initialise the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        // Wait for the next cycle.
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        ESP_LOGI("FREE_HEAP", "Free Heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    }
}

// Initialize Wi-Fi as sta and set scan method
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

    // Wait for connection
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
            pdMS_TO_TICKS(15000)); // 15 seconds timeout
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "WiFi connection TIMEOUT or UNKNOWN EVENT");
    }
    vEventGroupDelete(wifi_event_group);

    esp_wifi_set_promiscuous(true);

    // 1. Set the Management Filter
    wifi_promiscuous_filter_t m_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&m_filter);

    wifi_promiscuous_filter_t c_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_ctrl_filter(&c_filter);

    // Enable CSI capture
    wifi_csi_config_t csi_cfg = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
    };
    esp_wifi_set_csi_config(&csi_cfg);
    esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL); // Use the callback that publishes to MQTT
    esp_wifi_set_csi(true);

    ESP_LOGI(TAG, "WiFi Init and CSI setup complete");
    xTaskCreate(vLogFreeHeap, "LogFreeHeap", 2048, NULL, 5, NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI("MAIN", "App started");
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

<<<<<<< HEAD:main/receiver.c
    for (int i = 0; i < ACTIVE_SUBCARRIERS; i++) {
        std_vals[i] = 1.0f;
    }
=======
    // Create a task for TFLite initialization to avoid blocking/crashing the main task
    xTaskCreate(tflite_task, "tflite_task", 8192, NULL, 5, NULL);
>>>>>>> 86fa14964b9b6006860307a71435e5c54de93d99:main/receiver.cpp

    wifi_init_sta();

    esp_mqtt_client_config_t mqtt_cfg = {};
    memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    global_client = client;
    esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}
