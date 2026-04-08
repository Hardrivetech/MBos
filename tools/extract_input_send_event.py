#!/usr/bin/env python3
import json, sys
p='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
try:
    js=json.load(open(p,'r',encoding='utf-8'))
except Exception as e:
    print('failed load',e); sys.exit(2)
for item in js.get('return', []):
    if item.get('name')=='input-send-event':
        print(json.dumps(item, indent=2))
        sys.exit(0)
print('not found')
