# Changelog

All notable changes to hms-scale-esp are documented here.

The firmware version itself lives in `main/version.h` (`HMS_SCALE_ESP_VERSION`),
which is the single source of truth — the boot banner, the web UI and the mDNS
TXT record all read it from there. This file is the only place a version number
is written out separately.

## [2.0.2]

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

## [2.0.1]

- Only clear NVS on WiFi auth failure, not on transient WiFi drops.

## [2.0.0]

- BLE blocking, stack overflow and race fixes from the QA audit: webhook POSTs
  moved off the BLE callback onto a queue + dedicated task, `esp_timer` instead
  of `vTaskDelay` in the GATTC disconnect handler, 8 KB HTTP server stack.
- NVS corruption fixes: checked `nvs_set_*` return values, delay before
  `esp_restart()`, 24 KB NVS partition, exponential WiFi backoff.

## [1.0.0]

- Initial release: Etekcity scale BLE GATT client, HTTP webhook, captive
  portal with DNS hijack, NVS config.
