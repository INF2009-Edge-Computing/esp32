# Workspace Instructions for edge‑esp32 / inf2009_proj

This workspace consists of two cooperating components:

1. **ESP32 firmware** (`edge-esp32/main/receiver.c`) running on an ESP32‑C3 board.  It
   connects to Wi‑Fi, subscribes to MQTT control topics, captures Channel State
   Information (CSI) frames and uploads them in sub‑batches over HTTP to a Pi server.
2. **Pi‑side Python services** located under `inf2009_proj/` and `edge-esp32/`:
   - `dashboard.py` — a CustomTkinter UI & MQTT client for calibration management.
   - `server.py` — Flask ingest endpoint `/upload_data` that merges uploaded CSI.
   - `trigger_collect.py`, `broadcast_generator.py`, `testlog.py` — helper scripts.

## Build & run commands

-- **ESP32 firmware** (use ESP‑IDF v5.5.2):
  ```powershell
  cd edge-esp32
  idf.py set-target esp32c3            # once
  idf.py build                         # compile
  idf.py -p <PORT> flash              # flash to device
  idf.py -p <PORT> monitor            # view serial output
  ```
  The VS Code ESP‑IDF extension may set IDF_PATH automatically.

-- **Python environment** (on Pi or dev machine):
  ```bash
  cd inf2009_proj           # or edge-esp32 for Python scripts
  python3 -m venv .venv
  source .venv/bin/activate
  pip install flask numpy pandas paho-mqtt customtkinter
  python dashboard.py       # launch GUI
  python server.py          # HTTP ingest
  python trigger_collect.py # send collect command via MQTT
  ```

## Key conventions

-- MQTT topics are structured as `/commands/<NODE>/collect`, `/sensors/<NODE>/status`.
-- `ESP32_ID` and `RACK_ID` must match across firmware, dashboard, and scripts.
-- Calibration states defined by `CALIB_STATES` (e.g. "door_closed", etc.)
  are used as dictionary keys; they must be strings.
-- Firmware should never generate dummy traffic; sampling must rely on real
  ambient packets.  Earlier versions included a traffic task – **remove it**
  if present.

## Common tasks for the agent

-- **Adding new UI features**: modify `dashboard.py` views, persist config in
  `config.json`, use existing patterns for progress bar and state handling.
-- **Extending firmware**: edit `receiver.c` above the Wi‑Fi initialization, use
  `publish_with_retry` for MQTT, `esp_http_client` for upload; follow patterns
  already in place for sessions.
-- **Tests & debugging**: rely on `SETUP_TEST.md` for manual end‑to-end steps.
-- **Cross‑component changes**: always update MQTT topic constants in all
  relevant files (`receiver.c`, `dashboard.py`, `trigger_collect.py`).
-- **Remove or fix workarounds**: if you see `traffic_task`, UDP sockets, or any
  `dummy` data, that's a red flag; revert to real‑packet logic.

## Project structure highlights

```
edge-esp32/
  main/receiver.c           # firmware
  CMakeLists.txt
  server.py                 # Python ingest
inf2009_proj/
  dashboard.py             # GUI + MQTT client
  testlog.py               # MQTT traffic generator
```

-The rest of the repo contains build outputs, Python virtualenvs, etc.

## Additional notes

-- Sensitive values (`WIFI_SSID`, `WIFI_PASS`, Pi IP) are hard‑coded in source
  for convenience; treat them as editable constants when testing.
-- The `ESP_SYSTEM_USE_FRAME_POINTER` option may be enabled in SDKconfig for
  better crash traces; see README for build notes.
-- The `server.py` maintains an `active_sessions` dict; avoid breaking its
  interface when altering request headers.

## Deployment & updates

Whenever you update scripts or firmware, the code must be transferred
and flashed appropriately:

1. **Copying files to the Pi** – use `scp` with the password `password`:
   ```powershell
   scp -r <path> iankoh@raspberrypi.local:/home/iankoh/<dest_dir>
   # the tool will prompt for password; enter "password" when asked
   ```
   Example:
   ```powershell
   scp C:\Users\ianko\git\inf2009_proj\dashboard.py iankoh@raspberrypi.local:/home/iankoh/inf2009_proj/
   ```
   Repeat for any other updated Python file (e.g. `server.py`, `train_model.py`).

2. **Flashing firmware** – after editing `receiver.c`, rebuild and flash:
   ```powershell
   cd edge-esp32
   idf.py build
   idf.py -p <COM_PORT> flash
   ```
   Confirm the device reboots and prints connection logs.

3. **Remote update command** – the Pi command for the Python services is now:
   ```powershell
   scp C:\Users\ianko\git\inf2009_proj\server.py iankoh@raspberrypi.local:/home/iankoh/inf2009_proj/
   ```

These steps should be included in any PR description or agent workflow
whenever code affecting the Pi or ESP32 is changed.

----
*This file is intended for AI assistants working in this workspace.  Follow it
whenever you propose changes, run code, or guide a developer through tasks.*