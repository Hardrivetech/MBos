#!/usr/bin/env bash
set -euo pipefail
# Automated VNC pointer test for MBos
# Usage: run this from the project root under WSL. Requires .venv with vncdotool installed.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="$ROOT_DIR/.venv"
VNCDOTOOL="$VENV_DIR/bin/vncdotool"
SERIAL_LOG="$ROOT_DIR/build/serial.log"

if [ ! -x "$VNCDOTOOL" ]; then
    echo "vncdotool not found at $VNCDOTOOL"
    echo "Activate or create the virtualenv first: python3 -m venv .venv && . .venv/bin/activate && pip install vncdotool"
    exit 2
fi

start_lines=0
last_status_line=""
if [ -f "$SERIAL_LOG" ]; then
    start_lines=$(wc -l < "$SERIAL_LOG" || true)
    last_status_line=$(grep "STATUS:" "$SERIAL_LOG" | tail -n1 || true)
fi

echo "Sending VNC pointer events to 127.0.0.1::5901 (Start -> Terminal)"
"$VNCDOTOOL" -s 127.0.0.1::5901 --delay=200 move 12 738 click 1 move 24 468 click 1 || true
sleep 1

# Extract new serial output produced since the test started
newfile="/tmp/mbos_vnc_new.log"
if [ -f "$SERIAL_LOG" ]; then
    sed -n "$((start_lines+1)),\$p" "$SERIAL_LOG" > "$newfile" || true
else
    echo "No serial log found at $SERIAL_LOG" > "$newfile"
fi

echo "--- New serial output ---"
cat "$newfile" || true
echo "--- End new output ---"

if grep -q "MOUSE IRQ" "$newfile"; then
    echo "MOUSE IRQ detected in new output"
    mouse_ok=1
else
    echo "No MOUSE IRQ in new output"
    mouse_ok=0
fi

# Check whether STATUS coordinates changed in the new output (indicates mouse movement)
status_changed=0
if [ -n "$last_status_line" ]; then
    last_x=$(echo "$last_status_line" | sed -n 's/.*mouse_x=\([0-9]*\), y=\([0-9]*\).*/\1/p') || true
    last_y=$(echo "$last_status_line" | sed -n 's/.*mouse_x=\([0-9]*\), y=\([0-9]*\).*/\2/p') || true
    if [ -n "$last_x" ]; then
        if grep -q "STATUS: mouse_x=" "$newfile"; then
            while IFS= read -r sline; do
                nx=$(echo "$sline" | sed -n 's/.*mouse_x=\([0-9]*\), y=\([0-9]*\).*/\1/p') || true
                ny=$(echo "$sline" | sed -n 's/.*mouse_x=\([0-9]*\), y=\([0-9]*\).*/\2/p') || true
                if [ -n "$nx" ] && { [ "$nx" -ne "$last_x" ] || [ "$ny" -ne "$last_y" ]; }; then
                    status_changed=1
                    break
                fi
            done < <(grep "STATUS: mouse_x=" "$newfile" || true)
        fi
    fi
fi

if [ "$status_changed" -eq 1 ]; then
    echo "STATUS coordinates changed in new output"
fi

wm_ok=0
if grep -q "WM STATE" "$newfile"; then
    echo "WM STATE detected in new output"
    wm_ok=1
elif grep -q "WM STATE" "$SERIAL_LOG"; then
    echo "WM STATE found in overall serial log"
    wm_ok=1
else
    echo "No WM STATE found"
fi

# Consider the test successful if we saw IRQs or STATUS movement (WM_STATE optional)
if { [ "$mouse_ok" -eq 1 ] || [ "$status_changed" -eq 1 ]; }; then
    echo "RESULT: SUCCESS (mouse activity detected)"
    if [ "$wm_ok" -eq 1 ]; then
        echo "WM_STATE observed"
    else
        echo "WM_STATE not observed (GUI may need manual interaction)"
    fi
    exit 0
else
    echo "RESULT: FAILURE (no mouse activity detected)"
    exit 3
fi
