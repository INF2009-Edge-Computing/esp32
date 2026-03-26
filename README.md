# edge-esp32

> **Getting the code onto your Pi**
>
> 1. Install Git if it's not already present:
>
>    ```bash
>    sudo apt update && sudo apt install -y git
>    ```
>
> 2. Clone the repository into any directory you like:
>
>    ```bash
>    git clone https://github.com/dennytan-19/edge-esp32.git
>    cd edge-esp32
>    ```
>
> 3. From this directory you can run the Python servers, scripts, or build the ESP32 image.
>
End-to-end edge pipeline using an **ESP32-C3** (CSI capture + upload) and a **Raspberry Pi 5** (MQTT broker + HTTP ingest server).

This project works in two parts:

- **ESP32 firmware** (`main/receiver.cpp`) captures CSI packets and uploads data to the Pi.
- **Pi-side Python services** receive control commands (MQTT) and store uploaded CSI batches (HTTP).

---

## Architecture at a glance

1. ESP32 connects to Wi-Fi and MQTT broker on the Pi.
2. A command is published to `/commands/<RACK_ID>/collect`.
3. ESP32 collects CSI samples in sub-batches.
4. ESP32 uploads each sub-batch to `http://<pi-ip>:5000/upload_data`.
5. `server.py` merges all parts and saves final CSV in `csi_data/...`.

Optional:

- `broadcast_generator.py` sends UDP broadcast pulses (`INF2009`) at fixed intervals.

---

## Prerequisites

### Hardware

- ESP32-C3 board
- Raspberry Pi 5 (same network as ESP32)
- USB cable for flashing ESP32
- 2.4GHz Wi-Fi network/AP

### Software

### On Raspberry Pi 5

- Raspberry Pi OS (or any Linux distro)
- Python 3.9+
- `pip`
- Mosquitto broker + clients

### On development machine (used to flash ESP32)

- ESP-IDF **v5.5.2** (matches `sdkconfig` and `dependencies.lock`)
- Target: `esp32c3`
- Serial/JTAG access to board

> Note: VS Code ESP-IDF extension is optional but convenient.

---

## Project files you will use

- `main/receiver.cpp` — ESP32 firmware (Wi-Fi, MQTT, CSI, HTTP upload)
- `server.py` — Flask HTTP ingest endpoint (`/upload_data`)
- `trigger_collect.py` — sends MQTT collect command
- `broadcast_generator.py` — sends UDP broadcast packet repeatedly

---

## 1) Setup Raspberry Pi 5

Run these on the Pi:

```bash
sudo apt update
sudo apt install -y python3 python3-pip python3-venv mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

From the project root:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install flask numpy pandas paho-mqtt
```

---

## Configuration

### Python helper scripts (trigger_collect.py, testlog.py)

```bash
cp .env.example .env
# edit .env — set MQTT_BROKER, RACK_ID, HTTP_SERVER_BASE
```

### ESP32 firmware credentials

WiFi credentials, MQTT broker URI, HTTP server URL, and node ID are now set via menuconfig — they are **not** hardcoded in source:

```bash
cd edge-esp32
idf.py menuconfig
# Navigate to: CSI Node Configuration
# Set: Wi-Fi SSID, Wi-Fi Password, MQTT Broker URI, HTTP Server Base URL, Node ID
```

After setting values, build and flash normally:

```bash
idf.py build
idf.py -p <PORT> flash
```

`sdkconfig.defaults` documents the keys with placeholder comments.

---

## 3) Build and flash ESP32-C3

From the project root (on your ESP-IDF machine):

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <SERIAL_PORT> flash monitor
```

Examples:

- Windows serial port: `COM8`
- Linux serial port: `/dev/ttyUSB0` or `/dev/ttyACM0`

You should see logs indicating:

- Wi-Fi connected
- MQTT connected
- CSI setup complete

---

## 4) Start Pi-side runtime services

Open terminals on the Pi in project root (activate venv first if used).

### Terminal A — Start HTTP ingest server

```bash
python3 server.py
```

Server listens on `0.0.0.0:5000`.

### Terminal B — (Optional) Start UDP broadcast pulses

```bash
python3 broadcast_generator.py
```

### Terminal C — Trigger collection command

```bash
python3 trigger_collect.py
```

If running `trigger_collect.py` on the Pi, setting `MQTT_BROKER=localhost` in `.env` is fine.
If running it from another machine, set `MQTT_BROKER` to the Pi IP.

---

## 5) What happens during a collection

Current firmware settings:

- `SAMPLE_SIZE = 200`
- `SUB_BATCH_SIZE = 40`

So each collection is split into **5 sub-batches**.

`server.py` stores temporary parts under:

`csi_data/<esp32_id>/<room_state>/temp_session/part_XXX.csv`

After all parts arrive, it merges and writes final file:

`csi_data/<esp32_id>/<room_state>/csi_<room_state>_<timestamp>.csv`

---

## 6) Quick verification checklist

- ESP32 and Pi are on reachable network paths.
- MQTT Broker URI and HTTP Server Base URL are set correctly in `idf.py menuconfig`.
- Mosquitto is running on port `1883`.
- Flask server is running on port `5000`.
- `RACK_ID` in `.env` for `trigger_collect.py` matches the firmware Node ID.

---

## Troubleshooting

### ESP32 does not receive collect command

- Check topic naming mismatch (`RACK_1` vs other ID).
- Check MQTT broker URI in firmware menuconfig (`CSI Node Configuration`).
- Confirm broker is running: `sudo systemctl status mosquitto`.
---

## MQTT Topics and Payloads (firmware ↔ controller)

The project uses topic templates containing the node ID (e.g., `RACK_1`):

- `/commands/<node>/collect`
  - Firmware subscribes.
  - Dashboard and `trigger_collect.py` publish.
  - Payload: plain label (`"door_closed"`) or JSON `{ "label": "...", "session": "...", "campaign_id": "...", "run_id": "...", "split_group": "train|dev|test" }`.

- `/commands/<node>/training_complete`
  - Dashboard publishes after model training is complete.
  - Firmware subscribes and arms model download.
  - Payload: JSON with `session` (e.g., `{ "session": "202405071234" }`).

- `/commands/<node>/update_model`
  - Legacy firmware command for model download request.
  - Ignored by firmware until at least one `training_complete` arrives.

- `/commands/<node>/load_model`
  - Manual debug command from `push_model.py` to force immediate model download.

- `/sensors/<node>/status`
  - Firmware publishes status/ack/info.
  - Values are JSON (`model_ready`, `model_download_armed`, `event`: "ack" | "node_online" | "heartbeat" ...).
  - Server/dashboard subscribe for UI updates.

- `/sensors/<node>/perf_bin`
  - Firmware publishes binary performance telemetry (`PASO` profiling):
    - struct `<BBHIIIIIiiiiI>`
    - fields: version, reserved, payload_size, invoke_last_us, invoke_avg_us, invoke_min_us, invoke_max_us, sample_count, queue_wait_us, feature_us, invoke_stage_us, pipeline_total_us, free_heap.

- `device/<node>/status`
  - Firmware publishes retained presence status: `"online"` / `"offline"` (QoS 1, retain).
  - Broker LWT publishes retained `"offline"` on ungraceful disconnect.

### Dashboard liveliness behavior

- Dashboard consumes both retained `device/<node>/status` and `/sensors/<node>/status` heartbeat events.
- Nodes can show:
  - `ONLINE` (fresh heartbeat),
  - `STALE` (heartbeat timeout fallback),
  - `OFFLINE` (explicit offline or never seen).
- This improves responsiveness and avoids stale "always connected" indicators.

### Feature mapping

- Collection flow (CSI capture + upload): `/commands/<node>/collect` → FIRWARE CSI data batching → HTTP `/upload_data`.
- Model update flow (training pipeline): dashboard triggers `/commands/<node>/training_complete` + `/commands/<node>/update_model` → firmware downloads model via `/model/<node>` + `/params/<node>`.
- Manual model push: `push_model.py` uses `/commands/<node>/load_model` and server-side model store.
- Observability: `status` + `perf_bin` topics are used for UI state and performance monitoring.

---

## How to use the MQTT flow with this repo

1. Start Mosquitto and verify `MQTT_BROKER`/`MQTT_PORT`.
2. Run `python server.py`, then `python dashboard.py`.
3. Set `RACK_ID` in `.env` to match `CSI Node Configuration` in ESP32 firmware.
4. Invoke collection with `python trigger_collect.py` (or via dashboard UI).
5. Observe CSI upload progress and model training status in dashboard.
6. Check `mosquitto_sub -t "/sensors/+/status" -v` for real-time status streams.
- If the ESP32 log shows errors like `esp-tls: couldn't get hostname for :192.168.X.X`
  or `Cannot publish, MQTT not connected`, the URI parsing may have failed.
  You can avoid this by specifying the broker as host/port instead of
  `mqtt://...` (see `main/receiver.cpp`).
  Ensure you set the values in the nested `broker.address` struct; e.g.:  
  ```c
  esp_mqtt_client_config_t cfg = {
      .broker.address.hostname  = MQTT_BROKER_HOST,
      .broker.address.port      = MQTT_BROKER_PORT,
      .broker.address.transport = MQTT_TRANSPORT_OVER_TCP, // required
  };
  ```
- Whenever the Pi’s IP changes, re-run `idf.py menuconfig` and update
  MQTT broker URI / HTTP server base URL, then **rebuild and reflash the ESP32**.

### `server.py` prints `Wrong Size` / `DATA MISMATCH`

- Firmware/server constants must stay aligned:
  - subcarrier packing logic in `receiver.cpp`
	- `SUB_BATCH_SIZE` and expected payload logic in `server.py`

### No final merged CSV appears

- Not all parts arrived.
- Check ESP32 HTTP upload logs and `server.py` request logs.
- Confirm Pi firewall/network allows TCP 5000.

### No CSI activity in logs

- Ensure Wi-Fi is connected and CSI callback is active.
- Confirm traffic is present from target AP/BSSID.

### MQTT client appears silent

If you don't see any MQTT-related lines on the ESP32 console even though 
`MQTT Connected` shows up, check that the monitor is running at **INFO** level
(`idf.py monitor` default) and that the device was flashed with the latest
firmware (boot banner should include the most recent timestamp).  The
application now emits additional messages such as `MQTT_EVENT_BEFORE_CONNECT`,
`MQTT message published` and any error events; scroll or search the log for
`MQTT_` to confirm it's active.  You can also run a subscriber on the Pi to see
if messages actually arrive:

```bash
mosquitto_sub -h 10.198.7.22 -t "/sensors/RACK_1/status" -v
```

If you still see nothing, reflash the ESP32 and ensure the broker IP/port are
correct.

> **Note for Python scripts**: this repo uses `paho-mqtt==2.x`. Prefer
> explicit callback API selection:
> - `mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, ...)` for v2-style callbacks
> - `mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, ...)` for legacy callback signatures

---

## Current defaults in repository

- Python helper script defaults are in `.env.example`.
- Firmware configuration keys are documented in `sdkconfig.defaults`.
- Actual runtime firmware values are set via `idf.py menuconfig` under **CSI Node Configuration**.

Adjust these values to your environment before running.

---

## Feature mode flags (firmware)

`main/receiver.cpp` has two runtime flags that come from `scaler_params.json`:

- `notebook_mode_enabled`
  - Uses explicit subcarrier features plus `AVG_VARIATION`.
  - Selected when scaler metadata provides `feature_columns` as concrete `SC_<n>` columns.

- `grouped_mode_enabled`
  - Uses grouped/condensed features plus one variation feature.
  - Selected when scaler metadata says `feature_mode=grouped` and provides `group_count`.

If neither mode is selected, firmware falls back to legacy layout handling.

On model load success, firmware now publishes `feature_count` and `feature_mode` in the
`model_ready` status payload so server/dashboard logs can verify training/firmware alignment.
