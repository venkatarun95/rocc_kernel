import socket
import sys

port = int(sys.argv[1])

serversocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# bind the socket to a public host, and a well-known port
serversocket.bind(("0.0.0.0", port))  # socket.gethostname()
# become a server socket
serversocket.listen(5)
print("Listening")

try:
    while True:
        # accept connections from outside
        (clientsocket, address) = serversocket.accept()
        print("Connected: ", address)
        while True:
            chunk = clientsocket.recv(1024 * 32)
            if chunk == b'':
                print("Finished receiving data")
                break
except Exception:
    print("Closing server")
    serversocket.close()
