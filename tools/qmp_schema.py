#!/usr/bin/env python3
import socket,json,sys
HOST='127.0.0.1'; PORT=4444
try:
    s=socket.create_connection((HOST,PORT),timeout=2)
except Exception as e:
    print('connect failed:',e); sys.exit(2)
# greeting
try:
    print('greeting:', s.recv(8192).decode()[:200])
except Exception as e:
    print('greet err',e)
s.sendall(json.dumps({"execute":"qmp_capabilities"}).encode()+b"\n")
print('sent qmp_capabilities')
try:
    print('cap resp:', s.recv(8192).decode()[:200])
except Exception:
    pass
# request qmp schema
s.sendall(json.dumps({"execute":"query-qmp-schema"}).encode()+b"\n")
allb=b''
while True:
    try:
        part=s.recv(32768)
        if not part: break
        allb+=part
    except Exception:
        break
s.close()
print('len schema', len(allb))
try:
    js=json.loads(allb.decode('utf-8'))
    out='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
    with open(out,'w',encoding='utf-8') as f:
        json.dump(js, f)
    print('wrote', out)
except Exception as e:
    print('parse err', e)
    print(allb[:200])
