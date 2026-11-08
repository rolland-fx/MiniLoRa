// Payload v1 codec — the 28-byte contract.
//
// BYTE-EXACT with the v1 firmware (MiniLoRa.ino `buildUplinkPayload` at tag
// Before-Rework) for all in-range inputs. The single intentional divergence:
// v2 SATURATES the battery and accelerometer maps at range edges, where v1
// wrapped (mod-256 truncation). See docs/PHASE2_DOMAIN.md and
// docs/GOLDEN_VECTORS.md (vector `acc_saturation`).
//
// This file must stay ESP-IDF-free: it compiles on the host for unit tests.

#pragma once

#include <cstddef>
#include <cstdint>

#include "Telemetry.hpp"

namespace minilora {

inline constexpr size_t kPayloadSize = 28;
inline constexpr uint8_t kFportTelemetry = 1;
inline constexpr uint8_t kFportDiagnostics = 2;
inline constexpr uint8_t kFportHistory = 4;

/// Saturating Arduino-map equivalent: ((v - lo) * 255) / (hi - lo), clamped.
uint8_t mapToByte(int32_t v, int32_t lo, int32_t hi);

/// Battery 2500..4300 mV → 0..255 (saturating).
uint8_t batteryToByte(uint16_t mv);

/// Accelerometer raw counts −10000..10000 → 0..255 (saturating).
uint8_t accelToByte(int32_t raw);

/// Reverse maps (decoded ≈ value; quantization loss documented in vectors).
uint16_t batteryFromByte(uint8_t b);
int32_t accelFromByte(uint8_t b);

/// Serialize `t` into `out` (must hold kPayloadSize bytes). Returns kPayloadSize.
size_t encodePayload(const TelemetryData &t, uint8_t *out);

/// Parse a 28-byte frame. Returns true and fills `t` if `len == kPayloadSize`.
bool decodePayload(const uint8_t *in, size_t len, TelemetryData &t);

} // namespace minilora
