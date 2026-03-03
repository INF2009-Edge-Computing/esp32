#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
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

#define ESP32_ID RACK_1

#define STR(x) #x
#define XSTR(x) STR(x)
#define ESP32_COLLECT_TOPIC "/commands/" XSTR(ESP32_ID) "/collect"
#define ESP32_DOWNLOAD_TOPIC "/commands/" XSTR(ESP32_ID) "/update_model"
#define ESP32_STATUS_TOPIC "/sensors/" XSTR(ESP32_ID) "/status"

// Set your WiFi credentials here
#define WIFI_SSID "<WIFI_NAME>"
#define WIFI_PASS "<WIFI_PASSWORD>"

// Set your Pi 5 IP address here
#define MQTT_BROKER_URI "mqtt://192.168.1.9:1883"
#define HTTP_UPLOAD_URI "http://192.168.1.9:5000/upload_data"
#define DEFAULT_SCAN_LIST_SIZE 20

/* Define range of subcarriers to use for CSI, ignoring noisy/unused ones */
#define MAX_LOWER 4
#define MAX_UPPER 60
#define DC_NULL 32
#define NUM_SUBCARRIERS 64
#define EXPECTED_PERIOD_US 100000 // 10Hz expected period between packets
static int64_t last_trigger_us = 0; //Last time we accepted a packet for processing

#define SAMPLE_SIZE 200                                 // 20 seconds of data at 10Hz
#define SUB_BATCH_SIZE 40                               // How many packets of data store before sending to the server (to avoid large memory spikes)
uint8_t csi_buffer[SUB_BATCH_SIZE][MAX_UPPER - MAX_LOWER - 1]; // Store only amplitudes
u_int8_t packet_idx = 0;
u_int8_t sub_batch_idx = 0;
static bool is_collecting = false;
static char collection_label[12] = "unknown";
static uint8_t dynamic_target_mac[6];
static bool is_mac_locked = false;

static const char *TAG = "logger";
static esp_mqtt_client_handle_t global_client = NULL;
float baseline_amp[NUM_SUBCARRIERS] = {0};
bool wifi_connected = false;

typedef struct {
    char *label;
    u_int8_t sub_batch_idx;
    u_int8_t total_sub_batches;
} http_task_params_t;

esp_http_client_handle_t client;
esp_mqtt_client_handle_t mqtt_client;
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
    }
    
    // If this was the last sub-batch, clean up the HTTP client to free memory and notify server
    if (params->sub_batch_idx == params->total_sub_batches - 1) {
        esp_http_client_cleanup(client);
        client = NULL;

        char payload[64];
        snprintf(payload, sizeof(payload), "{\"event\": \"collection_complete\", \"label\": \"%s\"}",
                 params->label);
        esp_mqtt_client_publish(mqtt_client, ESP32_STATUS_TOPIC, payload, 0, 1, 0);
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

    if (memcmp(info->mac, dynamic_target_mac, 6) != 0)
        return;  // ignore non-router packets
    
    int64_t now = esp_timer_get_time();
    int64_t dt = now - last_trigger_us;

    // Only accept one packet per MIN_INTERVAL_US
    if (dt < EXPECTED_PERIOD_US)
        return; // too soon, ignore duplicates

    // Filter out outlier packets for consistent CSI data
    if (info->rx_ctrl.sig_len > 32 || info->rx_ctrl.rssi < -75 || info->rx_ctrl.rx_state != 0) 
        return;

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

    // Normalization Pass 2: Create the 'Feature Vector'
    uint8_t normalized_row[MAX_UPPER - MAX_LOWER - 1];
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
    ESP_LOGI(TAG, "CSI Packet: Avg Amp=%.2f", avg_amp);

    // --- PATH A: INFERENCE (Always Active) ---
    /* This is where you will eventually call:
       your_tflite_model_run(normalized_row);
    */

    // --- PATH B: DATA COLLECTION (Gated by MQTT) ---
    if (is_collecting)
    {
        // Copy the normalized row into the global BATCH buffer
        if (sub_batch_idx < SUB_BATCH_SIZE)
        {
            memcpy(csi_buffer[packet_idx], normalized_row, sizeof(normalized_row));
            packet_idx++;
        }

        // Check if the batch is complete
        if (packet_idx >= SUB_BATCH_SIZE) // Trigger upload at sub-batch size to avoid large memory spikes
        {
            ESP_LOGI(TAG, "Sub-batch full (%d/%d). Triggering upload for state: %s", sub_batch_idx + 1, (SAMPLE_SIZE / SUB_BATCH_SIZE), collection_label);

            http_task_params_t *params = malloc(sizeof(http_task_params_t));
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
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        // Subscribe to multiple topics
        esp_mqtt_client_subscribe(event->client, ESP32_COLLECT_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, ESP32_DOWNLOAD_TOPIC, 0);
        mqtt_client = event->client; // Store the client handle for use in the CSI callback
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received data on topic: %.*s", event->topic_len, event->topic);

        if (strncmp(event->topic, ESP32_COLLECT_TOPIC, event->topic_len) == 0)
        {
            // Payload could be the label, e.g., "empty" or "occupied"
            snprintf(collection_label, sizeof(collection_label), "%.*s", event->data_len, event->data);

            packet_idx = 0;       // Reset buffer position
            sub_batch_idx = 0;   // Reset sub-batch position
            is_collecting = true; // Start the data collection engine
            ESP_LOGI(TAG, "Starting data collection for state: %s", collection_label);
        } else if (strncmp(event->topic, ESP32_DOWNLOAD_TOPIC, event->topic_len) == 0)
        {
            // do your model download stuff here
            ESP_LOGI(TAG, "Model download requested!");
        }
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
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .listen_interval = 1,
        },
    };
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

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    wifi_init_sta();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    global_client = client;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}
