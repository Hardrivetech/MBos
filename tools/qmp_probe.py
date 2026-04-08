#!/usr/bin/env python3
import socket, json, sys
HOST='127.0.0.1'; PORT=4444
try:
    s=socket.create_connection((HOST,PORT),timeout=2)
except Exception as e:
    print('connect failed:',e); sys.exit(2)
# read greeting
try:
    g=s.recv(8192).decode('utf-8')
    print('greeting:',g[:200])
except Exception as e:
    print('greeting read err',e)
# send capabilities
s.sendall(json.dumps({"execute":"qmp_capabilities"}).encode()+b"\n")
try:
    r=s.recv(8192).decode()
    print('cap response:',r[:200])
except Exception as e:
    print('cap read err',e)
# query commands
s.sendall(json.dumps({"execute":"query-commands"}).encode()+b"\n")
allb=b''
while True:
    try:
        part=s.recv(8192)
        if not part: break
        allb+=part
        if len(allb)>200000: break
    except Exception:
        break
s.close()
print('query-commands length:',len(allb))
try:
    js=json.loads(allb.decode('utf-8'))
    # write to disk for inspection
    out='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_query_commands.json'
    with open(out,'w',encoding='utf-8') as f:
        json.dump(js, f, indent=2)
    print('wrote',out)
except Exception as e:
    print('json parse/write err',e)
    print(allb[:200])
