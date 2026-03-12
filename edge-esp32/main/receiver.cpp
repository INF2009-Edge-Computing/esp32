@@
 static void run_inference(uint8_t* csi_row, size_t size) {
     if (interpreter == nullptr || input == nullptr) return;
@@
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
@@
     const int STEADY_THRESHOLD = 5; // Log after 5 consecutive identical high-confidence packets
@@
         if (consecutive_count == STEADY_THRESHOLD) {
-            const char* labels[] = {"OPEN", "CLOSE", "PERSON"};
-            ESP_LOGW("STEADY_STATE", "Confirmed State: %s (Stability: %d pkts)", 
-                     labels[current_pred], STEADY_THRESHOLD);
+            // report a state_change event when the prediction stabilizes and
+            // differs from the last one we sent.  Map to the same strings used
+            // by the dashboard's CALIB_STATES.
+            const char* labels[] = {"door_open", "door_closed", "person_standing"};
+            ESP_LOGW("STEADY_STATE", "Confirmed State: %s (Stability: %d pkts)", 
+                     labels[current_pred], STEADY_THRESHOLD);
+
+            if (current_pred != last_reported_pred) {
+                last_reported_pred = current_pred;
+                cJSON *obj = cJSON_CreateObject();
+                if (obj) {
+                    cJSON_AddStringToObject(obj, "event", "state_change");
+                    cJSON_AddStringToObject(obj, "state", labels[current_pred]);
+                    char *json_str = cJSON_PrintUnformatted(obj);
+                    if (json_str) {
+                        publish_with_retry(ESP32_STATUS_TOPIC, json_str, 1);
+                        free(json_str);
+                    }
+                    cJSON_Delete(obj);
+                }
+            }
             consecutive_count = 0; // Reset after logging to avoid repeated spamming
         }
@@
 static bool is_collecting = false;
 static char collection_label[12] = "unknown";
 static uint8_t dynamic_target_mac[6];
 static bool is_mac_locked = false;
+// track last prediction sent to dashboard; reset when model or collection
+// changes so stale values do not persist
+static int last_reported_pred = -1;
@@
         if (strncmp(event->topic, ESP32_DOWNLOAD_TOPIC, event->topic_len) == 0)
         {
             // When an update message arrives we launch the combined downloader
             // which fetches both the TFLite model and the scaler parameters.
             ESP_LOGI(TAG, "Starting model+params download");
+            last_reported_pred = -1; // reset prediction state when we swap model
             xTaskCreate(download_model_and_params_task, "dl_mdl_params", 8192, NULL, 5, NULL);
         }
@@
             packet_idx = 0;  // Reset for the next MQTT trigger
             sub_batch_idx++; // Increment sub-batch index
@@
             if(sub_batch_idx >= (SAMPLE_SIZE / SUB_BATCH_SIZE)) {
                 sub_batch_idx = 0; // Reset sub-batch index after reaching max batches
                 is_collecting = false; // Stop collecting until next MQTT trigger
                 ESP_LOGI(TAG, "Collection complete for state: %s", collection_label);
+                last_reported_pred = -1; // clear detection state when calibration ends
             }
         }
