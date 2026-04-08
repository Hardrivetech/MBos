#!/usr/bin/env python3
import socket,json,sys
HOST='127.0.0.1'; PORT=4444
s=socket.create_connection((HOST,PORT),timeout=2)
print('greeting:', s.recv(8192).decode(errors='ignore')[:200])
s.sendall(json.dumps({"execute":"qmp_capabilities"}).encode()+b"\n")
print('cap:', s.recv(8192).decode(errors='ignore')[:200])
# Ask HMP for help on mouse
hmp = {"execute":"human-monitor-command","arguments":{"command-line":"help mouse"}}
s.sendall(json.dumps(hmp).encode()+b"\n")
allb=b''
while True:
    try:
        p=s.recv(8192)
        if not p: break
        allb+=p
    except Exception:
        break
s.close()
print(allb.decode(errors='ignore'))
