import socket
import time

# Use the broadcast address of your current network (usually .255)
# Or use 255.255.255.255 to hit everything on the subnet
BROADCAST_IP = "255.255.255.255" 
PORT = 5555

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xB8) # Force QOS high priority for sent packets

print(f"Sending broadcasts to {BROADCAST_IP}...")

try:
    while True:
        sock.sendto("INF2009".encode(), (BROADCAST_IP, PORT))
        time.sleep(0.075) # 75ms interval
except KeyboardInterrupt:
    print("Pulse stopped.")
