#!/usr/bin/env python3
import socket,sys,json
if len(sys.argv)<2:
    obj = {"execute": "query-mice"}
else:
    cmd = sys.argv[1]
    try:
        obj = json.loads(cmd)
    except Exception as e:
        print('invalid json',e); sys.exit(2)
HOST='127.0.0.1'; PORT=4444
s=socket.create_connection((HOST,PORT),timeout=2)
print('greeting:', s.recv(8192).decode(errors='ignore')[:200])
s.sendall(json.dumps({"execute":"qmp_capabilities"}).encode()+b"\n")
print('cap:', s.recv(8192).decode(errors='ignore')[:200])
s.sendall(json.dumps(obj).encode()+b"\n")
allb=b''
while True:
    try:
        p=s.recv(8192)
        if not p: break
        allb+=p
    except Exception:
        break
s.close()
print('resp:', allb.decode(errors='ignore'))
