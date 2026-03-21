#!/usr/bin/env python3
"""
Manual model pushing utility for debugging.

This script allows you to:
1. Upload a .tflite model file to the server without running full training
2. Optionally upload custom scaler parameters
3. Trigger the ESP32 to load the model immediately via MQTT

Usage:
    python push_model.py <model_file> [--node NODE_ID] [--params PARAMS_FILE] [--load]

Examples:
    # Push a model to RACK_3
    python push_model.py model.tflite --node RACK_3

    # Push model with custom scaler params and immediately trigger load
    python push_model.py model.tflite --params scaler_params.json --node RACK_3 --load

    # Just trigger load of existing model on server
    python push_model.py --node RACK_3 --load
"""

import os
import json
import shutil
import argparse
import sys
import paho.mqtt.client as mqtt
import time
from pathlib import Path

# Configuration (must match server and ESP32)
# For edge-esp32 directory, model store is in the parent inf2009_proj directory
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_STORE_DIR = os.path.join(REPO_ROOT, "inf2009_proj", "model_store")
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

# MQTT Topics (must match firmware)
def get_load_model_topic(node_id):
    return f"/commands/{node_id}/load_model"


def ensure_node_dir(node_id):
    """Ensure the node's model directory exists."""
    node_dir = os.path.join(MODEL_STORE_DIR, node_id)
    os.makedirs(node_dir, exist_ok=True)
    return node_dir


def validate_scaler_schema(params: dict) -> tuple[bool, str]:
    mean_vals = params.get("mean")
    std_vals = params.get("std")
    if not isinstance(mean_vals, list) or not isinstance(std_vals, list):
        return False, "scaler params must contain list fields: mean and std"
    if len(mean_vals) == 0 or len(std_vals) == 0:
        return False, "mean/std arrays cannot be empty"
    if len(mean_vals) != len(std_vals):
        return False, "mean/std arrays must be the same length"

    selected = params.get("selected_subcarriers")
    if isinstance(selected, list) and selected:
        if len(selected) + 1 != len(mean_vals):
            return (
                False,
                "selected_subcarriers length must satisfy len(mean)==len(selected_subcarriers)+1 for notebook features",
            )

    return True, "ok"


def push_model_file(model_path, node_id):
    """Upload model file to the server."""
    if not os.path.exists(model_path):
        print(f"ERROR: Model file not found: {model_path}")
        return False

    node_dir = ensure_node_dir(node_id)
    dest_path = os.path.join(node_dir, "model.tflite")

    try:
        # Backup existing model if it exists
        if os.path.exists(dest_path):
            backup_path = dest_path + ".backup"
            shutil.copy2(dest_path, backup_path)
            print(f"Backed up existing model to: {backup_path}")

        # Copy the new model
        shutil.copy2(model_path, dest_path)
        file_size = os.path.getsize(dest_path)
        print(f"✓ Model uploaded: {dest_path} ({file_size} bytes)")
        return True
    except Exception as e:
        print(f"ERROR: Failed to upload model: {e}")
        return False


def push_scaler_params(params_path, node_id):
    """Upload scaler parameters file to the server."""
    if not os.path.exists(params_path):
        print(f"ERROR: Scaler params file not found: {params_path}")
        return False

    node_dir = ensure_node_dir(node_id)
    dest_path = os.path.join(node_dir, "scaler_params.json")

    try:
        # Validate JSON format
        with open(params_path, 'r') as f:
            params = json.load(f)

        valid, reason = validate_scaler_schema(params)
        if not valid:
            print(f"ERROR: Invalid scaler schema: {reason}")
            return False

        # Backup existing params if they exist
        if os.path.exists(dest_path):
            backup_path = dest_path + ".backup"
            shutil.copy2(dest_path, backup_path)
            print(f"Backed up existing scaler params to: {backup_path}")

        # Copy the new params
        with open(dest_path, 'w') as f:
            json.dump(params, f, indent=2)

        file_size = os.path.getsize(dest_path)
        print(f"✓ Scaler params uploaded: {dest_path} ({file_size} bytes)")
        return True
    except json.JSONDecodeError as e:
        print(f"ERROR: Scaler params file is not valid JSON: {e}")
        return False
    except Exception as e:
        print(f"ERROR: Failed to upload scaler params: {e}")
        return False


def trigger_model_load_mqtt(node_id):
    """Send MQTT command to trigger model load on ESP32."""
    topic = get_load_model_topic(node_id)

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            print(f"Connected to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}")
            # Publish the load_model command
            result = client.publish(topic, "", qos=1, retain=False)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                print(f"✓ Triggered model load on {node_id} (topic: {topic})")
            else:
                print(f"ERROR: Failed to publish MQTT message: {result.rc}")
            client.disconnect()
        else:
            print(f"ERROR: Failed to connect to MQTT broker: {rc}")
            client.disconnect()

    def on_disconnect(client, userdata, rc):
        if rc == 0:
            print("Disconnected from MQTT broker")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect

    try:
        print(f"Connecting to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}...")
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        client.loop_start()
        time.sleep(2)  # Give time for connection and publish
        client.loop_stop()
        return True
    except Exception as e:
        print(f"ERROR: MQTT connection failed: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Push a model to the server and optionally trigger ESP32 load",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Examples:\n"
               "  python push_model.py model.tflite --node RACK_3\n"
               "  python push_model.py model.tflite --params params.json --node RACK_3 --load\n"
               "  python push_model.py --node RACK_3 --load  # trigger load of existing model\n",
    )

    parser.add_argument(
        "model_file",
        nargs="?",
        default=None,
        help="Path to the .tflite model file (optional if only triggering load)",
    )
    parser.add_argument(
        "--node",
        required=True,
        help="Node ID (e.g., RACK_3, RACK_2)",
    )
    parser.add_argument(
        "--params",
        default=None,
        help="Path to scaler_params.json file (optional)",
    )
    parser.add_argument(
        "--load",
        action="store_true",
        help="Trigger ESP32 to load the model via MQTT",
    )

    args = parser.parse_args()

    # Validate
    if not args.model_file and not args.load:
        print("ERROR: Must provide model_file or --load flag")
        parser.print_help()
        return 1

    success = True

    # Upload model if provided
    if args.model_file:
        print(f"Pushing model for node: {args.node}")
        success = push_model_file(args.model_file, args.node) and success

    # Upload scaler params if provided
    if args.params:
        print(f"Pushing scaler params for node: {args.node}")
        success = push_scaler_params(args.params, args.node) and success

    # Trigger load if requested
    if args.load:
        print(f"\nTriggering model load on {args.node}...")
        success = trigger_model_load_mqtt(args.node) and success

    if success:
        print("\n✓ All operations completed successfully!")
        return 0
    else:
        print("\n✗ Some operations failed. Please check the errors above.")
        return 1


if __name__ == "__main__":
    exit(main())
