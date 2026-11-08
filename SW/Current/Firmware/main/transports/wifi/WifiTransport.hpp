// WiFi transport — STA connect + HTTP POST of the 28-byte payload.
// Behavior-identical to v1 sendWifiPayload(): 10 s connect timeout,
// application/octet-stream POST to http://<ip>:<port>/data, then off.
// start()/stop() are exposed for the OTA client (same STA session).

#pragma once

#include <cstdint>
#include <cstddef>

namespace minilora {

class WifiTransport {
public:
    /// Bring up the STA and wait for an IP. Idempotent (safe to call when
    /// already up). NVS must be initialized (App::cycle guarantees it).
    static bool start(const char *ssid, const char *password,
                      uint32_t timeoutMs = 10'000);

    /// Park the radio (esp_wifi_stop). Safe when never started.
    static void stop();

    /// IP is stored LSB-first (octet 0 in the LSB) — v1 ABI quirk.
    /// deviceName rides the URL as ?device= so the server keys history by
    /// name, not by the (DHCP-changeable) client IP. `path` selects the
    /// server endpoint ("/data" telemetry, "/diag" diagnostics).
    static bool postPayload(const uint8_t *payload, size_t len,
                            const char *ssid, const char *password, uint32_t ip,
                            uint16_t port, const char *deviceName = "",
                            uint32_t timeoutMs = 10'000,
                            const char *path = "/data");
};

} // namespace minilora
