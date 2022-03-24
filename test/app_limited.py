import socket
import time
import sys

port = int(sys.argv[1])

TCP_CONGESTION = getattr(socket, 'TCP_CONGESTION', 13)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b'rocc')

s.connect(("100.64.0.1", port)) # Mahimahi address
# s.connect(("localhost", 5002))

msg_len = 1024 * 1024 * 8
msg = b"." * 1024 * 1024
totalsent = 0

try:
    while totalsent < msg_len:
        sent = s.send(msg)
        if sent == 0:
            raise RuntimeError("socket connection broken")
        totalsent += sent
    print("Sleeping")
    time.sleep(2)

    while totalsent < 2 * msg_len:
        sent = s.send(msg)
        if sent == 0:
            raise RuntimeError("socket connection broken")
        totalsent += sent

except Exception:
    s.close()
