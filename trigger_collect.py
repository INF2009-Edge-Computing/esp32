import paho.mqtt.client as mqtt 
import time 
 
# Configuration 
BROKER_IP = "localhost" 
BROKER_PORT = 1883 

# This should be set dynamically from the dashboard to trigger commands for specific receivers
RACK_ID = "RACK_1"

COLLECTION_TOPIC = f"/commands/{RACK_ID}/collect" 
MODEL_UPDATE_TOPIC = f"/commands/{RACK_ID}/update_model" 

def on_connect(client, userdata, flags, rc): 
    if rc == 0:
		# sensor/<DEVICE_ID>/status
        client.subscribe("/sensors/+/status")

        print("Connected! Sending collect command...") 
        # Send the command to the publisher 
        client.publish(COLLECTION_TOPIC, "door-closed") 
        
        # client.publish(MODEL_UPDATE_TOPIC, "update!")
    else: 
        print(f"Connection failed: {rc}") 

def on_message(client, userdata, message):
    print(f"Received message '{message.payload.decode()}' on topic '{message.topic}'")
    
client = mqtt.Client("ControllerNode")  
client.on_connect = on_connect 
client.on_message = on_message
 
client.connect(BROKER_IP, BROKER_PORT) 
client.loop_forever()
