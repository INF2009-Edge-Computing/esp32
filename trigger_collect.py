import os
import paho.mqtt.client as mqtt 
import time 
from dotenv import load_dotenv

load_dotenv()

# Configuration 
BROKER_IP = os.getenv("MQTT_BROKER", "localhost") 
BROKER_PORT = int(os.getenv("MQTT_PORT", "1883")) 

# This should be set dynamically from the dashboard to trigger commands for specific receivers
RACK_ID = os.getenv("RACK_ID", "RACK_1")

COLLECTION_TOPIC = f"/commands/{RACK_ID}/collect" 
MODEL_UPDATE_TOPIC = f"/commands/{RACK_ID}/update_model" 
TRAINING_COMPLETE_TOPIC = f"/commands/{RACK_ID}/training_complete"

def on_connect(client, userdata, flags, rc): 
    if rc == 0:
        # sensor/<DEVICE_ID>/status
        client.subscribe("/sensors/+/status")

        print("Connected! Sending collect command...") 
        # Send canonical state label + session payload
        payload = '{"label":"door_closed","session":"manual_test"}'
        client.publish(COLLECTION_TOPIC, payload)
        
        # client.publish(MODEL_UPDATE_TOPIC, "update!")
        # client.publish(TRAINING_COMPLETE_TOPIC, '{"session":"manual_train"}')
    else: 
        print(f"Connection failed: {rc}") 

def on_message(client, userdata, message):
    print(f"Received message '{message.payload.decode()}' on topic '{message.topic}'")
    
client = mqtt.Client("ControllerNode")  
client.on_connect = on_connect 
client.on_message = on_message
 
client.connect(BROKER_IP, BROKER_PORT) 
client.loop_forever()
