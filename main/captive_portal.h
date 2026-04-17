#pragma once

// Start captive portal AP mode with DNS hijack.
// Blocks until user saves config (WiFi + webhook URL) and device reboots.
void captive_portal_start(void);
