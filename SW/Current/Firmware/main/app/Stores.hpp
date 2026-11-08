// NVS-backed config + diagnostics persistence.
// v1 compatibility: same namespace "config", key "cfg" — devices configured
// by the v1 Android app keep their settings after the v2 update.

#pragma once

#include <cstdint>
#include <esp_err.h>

#include "../domain/Config.hpp"
#include "../domain/Diagnostics.hpp"

namespace minilora {

class ConfigStore {
public:
    /// Init NVS (defaults on fresh flash) and load. Returns true if a config
    /// was found; `cfg` is filled with defaults otherwise.
    static bool initAndLoad(DeviceConfig &cfg, const ConfigDefaults &defaults);

    /// Save through the versioned blob path.
    static bool save(const DeviceConfig &cfg);

    /// Overwrite from a BLE write (accepts legacy + v2 sizes).
    static bool applyWrite(const uint8_t *data, size_t len, DeviceConfig &cfg);
};

class DiagStore {
public:
    /// Increment boot counters; call once per boot. Handles the boot-loop
    /// guard threshold logic. Returns true if recovery mode is required.
    static bool onBoot(Diagnostics &diag);

    /// Mark the cycle completed cleanly (resets the unexpected counter).
    static void onCleanCycle();

    static void load(Diagnostics &diag);
    static void save(const Diagnostics &diag);
};

} // namespace minilora
