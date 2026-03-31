# edge-esp32

ESP32-C3 firmware plus helper scripts for CSI capture, HTTP upload, and model loading.

This repository is the device-side half of the system. It pairs with `inf2009_proj`, which runs the Pi-side server, dashboard, and training pipeline.

## What’s in here

| File / folder | Purpose |
| --- | --- |
| `main/receiver.cpp` | Core firmware: Wi-Fi, MQTT, CSI capture, feature extraction, inference, and HTTP upload |
| `main/tflm_inference.cpp` / `main/tflm_inference.h` | TensorFlow Lite Micro model loading and prediction helpers |
| `main/Kconfig.projbuild` | Firmware configuration menu for node ID plus up to four Wi-Fi/MQTT/HTTP profiles |
| `CMakeLists.txt` | ESP-IDF project entry point |
| `sdkconfig` / `sdkconfig.defaults` | Build configuration and defaults for ESP-IDF |
| `trigger_collect.py` | MQTT helper that publishes a collect command |
| `broadcast_generator.py` | Optional UDP broadcast helper for generating ambient traffic |
| `push_model.py` | Manual model upload helper that writes into the sibling `inf2009_proj/model_store/` tree |
| `.env.example` | Environment template for the Python helper scripts |
| `dependencies.lock` | Pinned ESP-IDF component versions |

## Summary

The firmware runs on an ESP32-C3 and does three main jobs:

1. connect to Wi-Fi and MQTT
2. capture CSI packets and upload them to the Pi in sub-batches over HTTP
3. load a matching model and scaler from the Pi when instructed

The Pi-side ingest server merges the uploaded sub-batches into the final CSV files under `inf2009_proj/csi_data/`.

## Setup

### Hardware and software prerequisites

- ESP32-C3 board
- Raspberry Pi or Linux machine running the Pi-side services from `inf2009_proj`
- USB cable for flashing
- ESP-IDF v5.5.2
- Python 3.9+ for the helper scripts

### Prepare the Python helper environment

If you plan to use `trigger_collect.py`, `broadcast_generator.py`, or `push_model.py`, create a small virtual environment and install the helper dependencies:

```bash
cd edge-esp32
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
pip install paho-mqtt python-dotenv
cp .env.example .env
```

Edit `.env` so it matches your network and node name:

- `MQTT_BROKER` — Pi IP or `localhost` if you run everything on one machine
- `MQTT_PORT` — usually `1883`
- `RACK_ID` — must match the firmware node ID
- `HTTP_SERVER_BASE` — for example `http://192.168.0.x:5000`

### Configure the firmware

Configure the ESP32 with `idf.py menuconfig`:

```bash
idf.py menuconfig
```

Set the CSI node configuration values to match your environment:

- `CSI_NODE_ID`
- `WIFI_SSID` / `WIFI_PASS` / `MQTT_BROKER_URI` / `HTTP_SERVER_BASE`
- optional fallback profiles `WIFI_SSID_2` ... `WIFI_SSID_4` with matching broker/base URL values

The firmware scans visible Wi-Fi networks, picks the first configured profile it can actually use, and only accepts a profile after the Pi server answers on `/health` and the existing MQTT model-load flow succeeds.

If you change any MQTT topic names or the feature layout, update both repositories together.

## Build and flash

From the `edge-esp32` directory:

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash
idf.py -p <PORT> monitor
```

Typical serial port examples:

- Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
- Windows: `COM8`

## How to use it

### Start collection from the helper script

Use the MQTT helper to trigger a CSI collection session:

```bash
python trigger_collect.py
```

### Upload a model and trigger a load

`push_model.py` copies the model and scaler into the sibling `inf2009_proj/model_store/<NODE>/` directory and can publish the `/commands/<NODE>/load_model` MQTT message.

```bash
python push_model.py model.tflite --node RACK_3
python push_model.py model.tflite --params scaler_params.json --node RACK_3 --load
python push_model.py --node RACK_3 --load
```

### Optional traffic helper

```bash
python broadcast_generator.py
```

## Things to note

- Model updates do not require reflashing the firmware; the ESP32 downloads the files from the Pi over HTTP.
- The current model-loading path expects the server to expose `/model/<NODE>` and `/params/<NODE>`.
- The Pi server also exposes `/health`; the firmware uses it during profile selection to verify the chosen HTTP endpoint before settling on that network.
- The firmware subscribes to `/commands/<NODE>/collect`, `/commands/<NODE>/stop_collect`, `/commands/<NODE>/training_complete`, and `/commands/<NODE>/load_model`.
- `/commands/<NODE>/stop_collect` is the preferred way to stop an active capture. Legacy `collect` payloads with `label=stop` are still accepted during rollout.
- For the current training pipeline, the feature layout must match the grouped metadata used by `inf2009_proj`.
- If model loading fails, check the serial monitor for `model_download_incompatible`, `model_download_memory_error`, or `model_download_failed` events.
- CSI collection relies on real network traffic; `broadcast_generator.py` is only an optional helper for testing.

## Quick workflow

1. Set up and start the Pi-side services in `inf2009_proj`.
2. Configure the ESP32 with `idf.py menuconfig`.
3. Build and flash the firmware.
4. Trigger collection with `trigger_collect.py` or the dashboard.
5. Train and export a model on the Pi side.
6. Copy the model into `model_store/<NODE>/` and trigger `/commands/<NODE>/load_model`.

For the Pi-side setup and training flow, see `../inf2009_proj/README.md`.
