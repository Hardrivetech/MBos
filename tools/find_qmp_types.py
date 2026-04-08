#!/usr/bin/env python3
import json
p='/mnt/c/Users/QuantumByte/Desktop/MBos/build/qmp_schema.json'
js=json.load(open(p,'r',encoding='utf-8'))
results=[]
for i,entry in enumerate(js.get('return', [])):
    if isinstance(entry, dict):
        name = entry.get('name','')
        meta = entry.get('meta-type','')
        if meta == 'type' or 'event' in name.lower() or 'input' in name.lower() or 'mouse' in name.lower():
            results.append((i,name,meta,entry))
for r in results[:200]:
    i,name,meta,entry = r
    print('INDEX',i,'NAME',name,'META',meta)
    print('  ',json.dumps(entry)[:200])
print('found',len(results),'matches')
