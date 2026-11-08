// Diagnostics uplink (fport 2) — sent every N cycles alongside fport 1.
//
// Schema v4 (28 bytes, little-endian). Byte 0 is the SCHEMA byte: decoders
// switch on it, never on the fw version. See SW/Current/ICD.md §2.
//
//   0     schema             = 0x04
//   1-3   fw version  major, minor, patch
//   4-5   bootCount         uint16
//   6-7   unexpectedResets  uint16
//   8     lastResetReason   uint8 (app-layer reset-reason code)
//   9-10  gpsTimeouts       uint16
//   11-12 loraJoinFailures  uint16
//   13-14 loraSendFailures  uint16
//   15-16 wifiFailures      uint16
//   17-18 zigbeeFailures    uint16
//   19-20 sensorReadFailures uint16
//   21    gpsConsecFails    uint8  (indoor-backoff counter)
//   22    powerFlags        uint8  bit0 = degraded, bit1 = critical
//   23    flags2            uint8  bit0 = gpsValid, bit1 = OTA pending,
//                                  bit2 = recovery mode
//   24    lastTtff          uint8  last successful TTFF s (255 unknown)
//   25    linkMargin        uint8  LinkCheck margin dB (255 unknown)
//   26    gwCnt             uint8  gateways that heard the last diag uplink
//   27    otaStatus         uint8  kOtaStatus*

#pragma once

#include <cstddef>
#include <cstdint>

#include "Diagnostics.hpp"

namespace minilora {

inline constexpr size_t kDiagPayloadSize = 29;
inline constexpr uint8_t kDiagSchema = 0x05;
inline constexpr uint8_t kDiagSendEveryCycles = 16; // ~1/day at 1.5 h interval

size_t encodeDiagnostics(const Diagnostics &d, uint8_t *out);

} // namespace minilora
