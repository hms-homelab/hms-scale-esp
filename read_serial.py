#!/usr/bin/env python3
import serial
import time
import sys

port = '/dev/cu.usbmodem21201'
baudrate = 115200
timeout_seconds = 30

try:
    ser = serial.Serial(port, baudrate, timeout=1)
    print(f"Opened {port} at {baudrate} baud")
    print("=" * 80)

    start_time = time.time()
    while time.time() - start_time < timeout_seconds:
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(line)
                sys.stdout.flush()
        time.sleep(0.01)

    print("=" * 80)
    print("Monitoring complete")
    ser.close()
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
