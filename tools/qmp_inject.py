#!/usr/bin/env python3
"""
Simple QMP injector to send input events to QEMU and help exercise PS/2 input path.
Run from WSL where QEMU exposes a QMP TCP socket on 127.0.0.1:4444.
"""
import socket
import sys
import time
import json

QMP_HOST = '127.0.0.1'
QMP_PORT = 4444

SERIAL_LOG = '/mnt/c/Users/QuantumByte/Desktop/MBos/build/serial.log'


def recv_json(sock, timeout=1.0):
    sock.settimeout(timeout)
    try:
        data = sock.recv(4096)
        if not data:
            return None
        s = data.decode('utf-8', errors='ignore')
        # QMP may send multiple JSON objects; try to find a JSON object
        return s.strip()
    except socket.timeout:
        return None


def send_cmd(sock, obj):
    j = json.dumps(obj)
    sock.sendall(j.encode('utf-8') + b"\n")


if __name__ == '__main__':
    print('QMP injector: connecting to {}:{}...'.format(QMP_HOST, QMP_PORT))
    try:
        s = socket.create_connection((QMP_HOST, QMP_PORT), timeout=2.0)
    except Exception as e:
        print('ERROR: cannot connect to QMP:', e)
        sys.exit(2)

    # Read greeting
    g = recv_json(s, timeout=2.0)
    print('QMP greeting:', g)

    # Send capabilities
    send_cmd(s, {"execute": "qmp_capabilities"})
    resp = recv_json(s, timeout=2.0)
    print('qmp_capabilities response:', resp)

    candidates = [
        {"execute": "input-send-event", "arguments": {"events": [{"type": "absolute", "data": {"x": 100, "y": 100}}]}},
        {"execute": "input-send-event", "arguments": {"events": [{"type": "relative", "data": {"dx": 100, "dy": 0}}]}},
        {"execute": "input-send-event", "arguments": {"events": [{"type": "relative", "data": {"dx": 0, "dy": 100}}]}},
        {"execute": "input-send-event", "arguments": {"events": [{"type": "relative", "data": {"dx": 50, "dy": 50}}, {"type": "button", "data": {"down": 1, "button": 1}}, {"type": "button", "data": {"down": 0, "button": 1}}]}},
    ]

    print('Trying candidate input events...')
    for idx, cmd in enumerate(candidates):
        print('\n==> Candidate', idx + 1, json.dumps(cmd))
        try:
            send_cmd(s, cmd)
        except Exception as e:
            print('send cmd failed:', e)
            continue
        # read immediate response
        r = recv_json(s, timeout=1.0)
        print('response:', r)
        # give guest a moment to process and emit serial output
        time.sleep(0.5)
        try:
            with open(SERIAL_LOG, 'rb') as f:
                f.seek(-4096, 2)
        except Exception:
            # file may be smaller or missing; read from start
            try:
                with open(SERIAL_LOG, 'rb') as f:
                    data = f.read().decode('utf-8', errors='ignore')
            except Exception as e:
                print('Cannot read serial log:', e)
                data = ''
        else:
            try:
                data = f.read().decode('utf-8', errors='ignore')
            except Exception:
                data = ''
        # Consider injection successful only if IRQ-level or VGA mouse lines appear
        if 'MOUSE IRQ' in data or 'MOUSE VGA' in data:
            print('Serial log shows mouse activity (snippet):')
            lines = [l for l in data.splitlines() if ('MOUSE' in l or 'mouse' in l or 'WM STATE' in l)]
            for L in lines[-20:]:
                print(L)
            print('Injection likely succeeded.')
            s.close()
            sys.exit(0)
        else:
            print('No mouse activity detected in serial log for this candidate.')

    print('\nAll candidates tried; no mouse IRQ activity detected in serial log.')
    s.close()
    sys.exit(1)
