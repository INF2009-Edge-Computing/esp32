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
+End-to-end edge pipeline using an **ESP32-C3** (CSI capture + upload) and a **Raspberry Pi 5** (MQTT broker + HTTP ingest server).

This project works in two parts:

- **ESP32 firmware** (`main/receiver.c`) captures CSI packets and uploads data to the Pi.
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

- `main/receiver.c` — ESP32 firmware (Wi-Fi, MQTT, CSI, HTTP upload)
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

## 2) Configure ESP32 firmware

Open `main/receiver.c` and update these values:

- `WIFI_SSID`
- `WIFI_PASS`
- `MQTT_BROKER_URI` → Pi IP, e.g. `mqtt://10.198.7.22:1883`
- `HTTP_UPLOAD_URI` → Pi IP, e.g. `http://10.198.7.22:5000/upload_data`

Also note:

- `#define ESP32_ID RACK_1`
- `trigger_collect.py` must use the same rack id (`RACK_ID = "RACK_1"`)

If they do not match, commands will not reach the device.

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
```tri

If running `trigger_collect.py` on the Pi, `BROKER_IP = "localhost"` is fine.
If running it from another machine, set `BROKER_IP` to the Pi IP.

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
- `MQTT_BROKER_URI` and `HTTP_UPLOAD_URI` point to Pi IP.
- Mosquitto is running on port `1883`.
- Flask server is running on port `5000`.
- `RACK_ID` in `trigger_collect.py` matches `ESP32_ID` in firmware.

---

## Troubleshooting

### ESP32 does not receive collect command

- Check topic naming mismatch (`RACK_1` vs other ID).
- Check MQTT broker IP/port in `receiver.c`.
- Confirm broker is running: `sudo systemctl status mosquitto`.
- If the ESP32 log shows errors like `esp-tls: couldn't get hostname for :192.168.X.X`
  or `Cannot publish, MQTT not connected`, the URI parsing may have failed.
  You can avoid this by specifying the broker as host/port instead of
  `mqtt://...` (see the top of `main/receiver.c`).
  Ensure you set the values in the nested `broker.address` struct; e.g.:  
  ```c
  esp_mqtt_client_config_t cfg = {
      .broker.address.hostname  = MQTT_BROKER_HOST,
      .broker.address.port      = MQTT_BROKER_PORT,
      .broker.address.transport = MQTT_TRANSPORT_OVER_TCP, // required
  };
  ```
- Whenever the Pi’s IP changes (e.g. now `10.198.7.22`), update the
  `MQTT_BROKER_HOST`/`MQTT_BROKER_PORT` (and `HTTP_UPLOAD_URI`) in `receiver.c`,
  then **rebuild and reflash the ESP32** so the firmware uses the new address.

### `server.py` prints `Wrong Size` / `DATA MISMATCH`

- Firmware/server constants must stay aligned:
	- subcarrier packing logic in `receiver.c`
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

> **Note for Python scripts**: recent `paho-mqtt` releases require passing
> `callback_api_version=1` when constructing the client.  Older versions
> defaulted to 1 automatically; if you encounter
> ``ValueError: Unsupported callback API version: version 2.0`` update the
> scripts (see `trigger_collect.py`) or install `paho-mqtt==1.6.1` in your
> virtual environment.

---

## Current defaults in repository

- MQTT broker URI in firmware: `mqtt://10.198.7.22:1883`
- HTTP upload URI in firmware: `http://10.198.7.22:5000/upload_data`
- Trigger script broker default: `localhost`

Adjust these values to your environment before running.

