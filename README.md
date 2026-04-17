# hms-scale-esp

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![experimental](https://img.shields.io/badge/status-experimental-orange)

ESP32-C3 BLE gateway for the Etekcity ESF-551 smart scale. Reads weight and impedance via BLE, sends measurements to [hms-scale](https://github.com/hms-homelab/hms-scale) via HTTP webhook. Captive portal for WiFi setup.

## Features

- BLE client for Etekcity ESF-551 scale (22-byte notification protocol)
- HTTP webhook delivery to hms-scale (replaces MQTT)
- Captive portal for first-boot WiFi + webhook URL configuration
- Station-mode web config UI at port 80
- NVS persistent storage for WiFi credentials and settings
- On-device body composition (Deurenberg) for local logging
- DNS hijacking for captive portal auto-redirect
- 3 retries with 2s backoff on webhook failure

## Architecture

```
Etekcity BLE Scale
    |
    | BLE notifications (22-byte packets)
    v
ESP32-C3 (this firmware)
    |
    | HTTP POST {"weight_kg", "weight_lb", "impedance", "battery"}
    v
hms-scale (C++ service)
    |
    +-- User identification (Random Forest ML)
    +-- BIA body composition (Kyle, Janssen, Watson)
    +-- PostgreSQL storage
    +-- MQTT publish to Home Assistant
```

## Quick Start

### 1. Build

```bash
# Set up ESP-IDF v5.3+
. ~/esp/esp-idf/export.sh

# Build
idf.py set-target esp32c3
idf.py build
```

### 2. Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### 3. Configure

On first boot (no WiFi configured):
1. Connect to WiFi AP: **GiraffeScale-XXXX**
2. Browser auto-redirects to setup page
3. Select your WiFi network
4. Enter WiFi password
5. Set hms-scale webhook URL (e.g., `http://192.168.x.x:8889/api/webhook/measurement`)
6. Click **Save & Connect** -- device reboots and connects

### 4. Runtime Config

Browse to the ESP32's IP address (port 80) to:
- View current WiFi, IP, BLE status
- Change webhook URL
- Factory reset (returns to captive portal)

## Configuration

Build-time options via `idf.py menuconfig`:

| Option | Default | Description |
|--------|---------|-------------|
| `SCALE_MAC_ADDRESS` | `D0:4D:00:51:4F:8F` | BLE MAC of the Etekcity scale |
| `DEFAULT_WEBHOOK_URL` | `http://hms-scale.local:8889/...` | Pre-filled webhook URL |
| `USER_AGE` | 30 | Age for on-device body metrics |
| `USER_HEIGHT_CM` | 175 | Height for on-device body metrics |
| `USER_SEX_MALE` | yes | Sex for on-device body metrics |

Runtime options (stored in NVS, configured via captive portal or web UI):
- WiFi SSID + password
- Webhook URL

## Webhook

**POST payload:**
```json
{
  "weight_kg": 71.89,
  "weight_lb": 158.49,
  "impedance": 537,
  "battery": 95
}
```

**Response from hms-scale:**
```json
{
  "status": "ok",
  "user": "Albin",
  "confidence": 95.0,
  "method": "deterministic_exact_match",
  "identified": true
}
```

## Hardware

- **MCU:** ESP32-C3 Super Mini
- **Scale:** Etekcity ESF-551 Smart Fitness Scale
- **BLE:** Bluedroid stack, GATT client
- **Sensors:** Load cell (weight in grams), BIA (impedance in ohms)

## Related Projects

- [hms-scale](https://github.com/hms-homelab/hms-scale) -- C++ smart scale backend service
- [hms-shared](https://github.com/hms-homelab/hms-shared) -- shared C++ libraries

## License

MIT License -- see [LICENSE](LICENSE) for details.
