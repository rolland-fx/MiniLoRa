// Zigbee transport — sleepy end device (ESP_ZB_HA_SIMPLE_SENSOR), one
// endpoint exposing the MiniLoRa dataset as native HA clusters:
//   ep 1: Basic, Power Config (battery), Temperature, Relative Humidity,
//         Pressure, Illuminance, Occupancy Sensing (motion from accel)
// Home Assistant (Z2M/ZHA) auto-discovers these — no custom converter.
//
// Persistence: ZBOSS NVRAM lives in the `zb_storage` flash partition and is
// written before deep sleep; on wake the stack restores the network context
// (child aging timeout set to 8192 min so short 15-min sleeps don't expire).

#pragma once

#include <cstdint>

#include "../../domain/Telemetry.hpp"

namespace minilora {

class ZigbeeTransport {
public:
    /// Start the stack (restores NVRAM context) and wait for the network.
    /// Returns true when joined. Safe to call every cycle: if already
    /// joined, the wait returns almost immediately.
    static bool startAndJoin(uint32_t timeoutMs);

    /// Push the dataset into cluster attributes and send report commands.
    /// `prevAcc` is the last cycle's accel vector (motion detection).
    static bool publish(const TelemetryData &t, const int16_t prevAcc[3]);

    /// Persist NVRAM. Call before prepareSleep().
    static void persist();

private:
    static void ensureTask();
};

} // namespace minilora
