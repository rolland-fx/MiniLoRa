// OTA via the MiniLoRa-server (device-pull).
//
// Protocol (see docs/LORAWAN.md and the server README):
//   GET http://<server>/ota/check?device=<devEUI>&current=<fw>
//     -> 200 { "update": true, "version": "2.5.0", "size": N,
//              "sha256": "<hex>", "path": "/ota/download/<id>" }
//     -> 200 { "update": false } (or 404 when unknown device)
//   GET http://<server>/ota/download/<id>  (streamed .bin, expected size+sha)
//
// The image is written to the passive slot and SHA-256-verified BEFORE the
// boot partition is switched; any mismatch/abort leaves the running image
// untouched. Plain HTTP is acceptable on the LAN because the image itself
// is authenticated by its digest — but only enable the check on networks
// you control.

#pragma once

#include "../domain/Config.hpp"
#include "../domain/Diagnostics.hpp"

namespace minilora {

class Ota {
public:
    /// Check the server (cfg.wifiServerIP:port) for an update and, if one
    /// is offered, download + verify + stage it. Returns true when a new
    /// image is staged and boot was switched (caller should esp_restart()).
    /// Returns false when up to date, unreachable, or on ANY error — the
    /// running image is never touched in that case. Reboots are the
    /// caller's job. `*statusOut` receives a kOtaStatus* code (diag v4).
    static bool apply(const DeviceConfig &cfg, uint8_t *statusOut = nullptr);
};

} // namespace minilora
