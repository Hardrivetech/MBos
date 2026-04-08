#!/bin/bash
pkill -9 -f qemu || true
sleep 1
cd /mnt/c/Users/QuantumByte/Desktop/MBos
# Ensure serial log exists and is writable from WSL
touch build/serial.log
chmod 666 build/serial.log
HDA_ARG=""
if [ -f build/disk.img ]; then
	HDA_ARG="-hda build/disk.img"
fi
nohup /usr/bin/qemu-system-i386 -cdrom build/mbos.iso $HDA_ARG -serial file:/mnt/c/Users/QuantumByte/Desktop/MBos/build/serial.log -monitor none -no-reboot -display none -vnc 127.0.0.1:1 >/dev/null 2>&1 &
echo $!
