import json
import os

import paho.mqtt.client as mqtt
from dotenv import load_dotenv

load_dotenv()

# Configuration
BROKER_IP = os.getenv("MQTT_BROKER", "localhost")
BROKER_PORT = int(os.getenv("MQTT_PORT", "1883"))

# This should be set dynamically from the dashboard to trigger commands for specific receivers.
RACK_ID = os.getenv("RACK_ID", "RACK_1")

COLLECTION_TOPIC = f"/commands/{RACK_ID}/collect"
MODEL_UPDATE_TOPIC = f"/commands/{RACK_ID}/update_model"
TRAINING_COMPLETE_TOPIC = f"/commands/{RACK_ID}/training_complete"


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        client.subscribe("/sensors/+/status", qos=1)
        client.subscribe("device/+/status", qos=1)

        print("Connected! Sending collect command...")
        # Send canonical state label + session payload expected by firmware/server.
        payload = json.dumps({"label": "door_closed", "session": "manual_test"})
        info = client.publish(COLLECTION_TOPIC, payload, qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            print(f"Publish failed (rc={info.rc}) on topic {COLLECTION_TOPIC}")

        # client.publish(MODEL_UPDATE_TOPIC, '{"session":"manual_test"}', qos=1, retain=False)
        # client.publish(TRAINING_COMPLETE_TOPIC, '{"session":"manual_train"}', qos=1, retain=False)
    else:
        print(f"Connection failed: {reason_code}")


def on_message(client, userdata, message):
    topic = message.topic.strip()
    payload = message.payload.decode(errors="replace").strip()

    if topic.startswith("device/") and topic.endswith("/status"):
        node_id = topic.split("/")[1]
        print(f"[device] {node_id}: {payload}")
        return

    if topic.startswith("/sensors/") and topic.endswith("/status"):
        try:
            msg = json.loads(payload)
        except json.JSONDecodeError:
            print(f"[status/raw] {topic}: {payload}")
            return
        evt = msg.get("event", "unknown")
        print(f"[status] {topic} event={evt} payload={msg}")
        return

    print(f"Received message '{payload}' on topic '{topic}'")


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ControllerNode")
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER_IP, BROKER_PORT, 60)
client.loop_forever()
