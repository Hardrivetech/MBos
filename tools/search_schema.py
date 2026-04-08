#!/usr/bin/env python3
import json,sys
p='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
js=json.load(open(p,'r',encoding='utf-8'))
found=False
# search return list for names
for entry in js.get('return', []):
    if isinstance(entry, dict) and entry.get('name')=='input-send-event':
        print('FOUND COMMAND:', json.dumps(entry, indent=2))
        found=True
        break
if not found:
    print('input-send-event not found')
