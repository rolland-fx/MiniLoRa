// BLE transport — two modes:
//   1. Telemetry beacon: extended advertising, LE Coded PHY, 28-byte payload
//      as manufacturer data (v1-compatible; Android scans it).
//   2. Config service: legacy 1M connectable adv, service 0000FACE…,
//      characteristic 0000CAFE… — reads return the legacy 158-byte blob
//      (v1 ABI), writes accept 158 (legacy), 159 (v2) or the 163-byte v3
//      envelope with CRC16 (see SW/Current/ICD.md §4).
//      Characteristic 0000CAFF… (read-only, 16 B) reports live device
//      status (ICD §5); the v1 app never touches it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../domain/Config.hpp"

namespace minilora {

class BleTransport {
public:
    /// Blocking: advertise the payload for `durationMs` on coded PHY.
    static void beacon(const uint8_t *payload, size_t len, const char *name,
                       uint32_t durationMs);

    struct ConfigResult {
        bool written = false;      // a client wrote a config
        DeviceConfig cfg{};
    };

    /// Provider invoked on every status read; fills `out` (kStatusSize
    /// bytes) and returns the length, or 0 if status is unavailable.
    /// Implemented by the app layer (fresh battery, diag, join state).
    static constexpr size_t kStatusSize = 16;
    using StatusProvider = size_t (*)(uint8_t *out);

    /// Blocking: run the config GATT service for `timeoutMs`.
    static ConfigResult runConfigService(const DeviceConfig &current,
                                         const char *name, uint32_t timeoutMs,
                                         StatusProvider status = nullptr);
};

} // namespace minilora
