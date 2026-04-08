#!/usr/bin/env python3
import json,sys
p='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
js=json.load(open(p,'r',encoding='utf-8'))
print('top keys:', list(js.keys()))
# If 'types' in top-level
if 'types' in js:
    print('types count', len(js['types']))
    # try to find type id 137
    for t in js['types']:
        if isinstance(t, dict) and t.get('name'):
            pass
    # try numeric index
    try:
        t137 = js['types'][137]
        print('type 137 keys:', list(t137.keys()))
        print(json.dumps(t137, indent=2)[:1000])
    except Exception as e:
        print('no index 137:', e)
else:
    print('no types key; dumping keys for inspection')
    for k in js.keys():
        print(k)
