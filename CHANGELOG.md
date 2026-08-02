# Changelog

All notable changes to hms-scale-esp are documented here.

The firmware version itself lives in `main/version.h` (`HMS_SCALE_ESP_VERSION`),
which is the single source of truth — the boot banner, the web UI and the mDNS
TXT record all read it from there. This file is the only place a version number
is written out separately.

## v2.0.3

### Fixed
- **OTA rebooted the device mid-upload.** `esp_ota_begin()` was passed
  `OTA_SIZE_UNKNOWN`, which erases the ENTIRE partition — `ota_0` is 0x1E0000
  (1.875 MB), and erasing more than ~1280 K takes over 5 s, tripping the default
  task watchdog (espressif/esp-idf#578). It does not look like a watchdog reset
  from the client side: the client keeps filling TCP buffers while the device is
  blocked in the erase, so the upload appears to stall around 130 KB after ~20 s.
  Now passes the real `req->content_len`, so only the needed sectors are erased.
  The OTA path shipped in 2.0.2 could not complete an upload; **2.0.3 must be
  flashed over USB**, since the broken OTA cannot deliver its own fix.
- OTA receive buffer is now 4 KB (one flash sector) instead of 2 KB, matching
  the proven cpapdash-push-c3 loop.

## v2.0.2

### Added
- OTA firmware updates over HTTP. `POST /ota` streams the image into the
  inactive slot via `esp_ota_*`, verifies it, sets the boot partition and
  reboots. A "Firmware Update" card on the status page uploads a `.bin`.
- Dual-OTA (A/B) partition table: `nvs`, `otadata`, `phy_init`, `ota_0`,
  `ota_1`. The first OTA-capable image must be USB-flashed once per board to
  lay down this table; updates thereafter are over the air.
- mDNS: the board advertises `giraffe-scale.local` plus an `_http._tcp`
  service on port 80 with `fw` and `device` TXT records, so the config UI is
  reachable without a router lookup. Uses the `espressif/mdns` managed
  component (see `main/idf_component.yml`) — mDNS is not an IDF core
  component in v5.x.
- `main/version.h` as the single source of truth for the firmware version.

### Fixed
- The boot banner printed a hardcoded `v2.0.1` regardless of the actual build.
  It now reads `HMS_SCALE_ESP_VERSION`.

## v2.0.1

- Only clear NVS on WiFi auth failure, not on transient WiFi drops.

## v2.0.0

- BLE blocking, stack overflow and race fixes from the QA audit: webhook POSTs
  moved off the BLE callback onto a queue + dedicated task, `esp_timer` instead
  of `vTaskDelay` in the GATTC disconnect handler, 8 KB HTTP server stack.
- NVS corruption fixes: checked `nvs_set_*` return values, delay before
  `esp_restart()`, 24 KB NVS partition, exponential WiFi backoff.

## v1.0.0

- Initial release: Etekcity scale BLE GATT client, HTTP webhook, captive
  portal with DNS hijack, NVS config.
