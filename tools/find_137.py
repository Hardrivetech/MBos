#!/usr/bin/env python3
import json,sys
p='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
js=json.load(open(p,'r',encoding='utf-8'))
for i,entry in enumerate(js.get('return', [])):
    s = json.dumps(entry)
    if '137' in s:
        print('index',i)
        print(s[:1000])
        break
else:
    print('no 137 found')
