// Field diagnostics — counters that ride the fport-2 uplink (Phase 6).
// Pure data + helpers: host-testable, no IDF dependency.

#pragma once

#include <cstdint>

namespace minilora {

struct Diagnostics {
    // Identity
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;

    // Boot health
    uint16_t bootCount;          // total boots since last factory reset
    uint16_t unexpectedResets;   // consecutive non-clean resets (boot-loop guard)
    uint8_t  lastResetReason;    // app-layer reset-reason code

    // Transport failures (saturating at 65535)
    uint16_t gpsTimeouts;
    uint16_t loraJoinFailures;
    uint16_t loraSendFailures;
    uint16_t wifiFailures;
    uint16_t zigbeeFailures;
    uint16_t sensorReadFailures;

    // Power/GPS strategy state (P1/P4)
    uint8_t  gpsConsecFails;     // consecutive GPS windows without fix (backoff)
    uint8_t  powerFlags;         // bit0 = degraded (low battery), bit1 = critical
    // Cycle-local flags (not persisted meaningfully; set fresh each cycle)
    uint8_t  gpsValid;           // this cycle's telemetry has a GPS fix
    uint8_t  imageFlags;         // kDiagFlagOtaPending | kDiagFlagRecovery
    // Field-debugging extras (diag v4, bytes 24-27)
    uint8_t  lastTtff;           // last successful TTFF in s (255 = unknown)
    uint8_t  linkMargin;         // LinkCheck margin dB from the PREVIOUS diag uplink (255 = unknown)
    uint8_t  gwCnt;              // gateways that heard it (same provenance)
    uint8_t  otaStatus;          // kOtaStatus* — result of the last OTA attempt
    // Remote-config state (diag v5, byte 28): the transport/sensor enables
    // as set in the runtime config. LoRaWAN is always on and not here.
    uint8_t  configFlags;        // kCfgFlag* bits (0 pre-v5 frames)
};

// Diag frame byte 28 "configFlags" (schema 0x05): the runtime config's
// transport/sensor enables, mirroring the ICD §3 CFG_SET flag ids 1-7.
inline constexpr uint8_t kCfgFlagWifi    = 0x01; // id 1
inline constexpr uint8_t kCfgFlagZigbee  = 0x02; // id 2
inline constexpr uint8_t kCfgFlagBle     = 0x04; // id 3 (beacon; config is always available)
inline constexpr uint8_t kCfgFlagGnss    = 0x08; // id 4
inline constexpr uint8_t kCfgFlagBme280  = 0x10; // id 5
inline constexpr uint8_t kCfgFlagOpt3001 = 0x20; // id 6
inline constexpr uint8_t kCfgFlagLis3dh  = 0x40; // id 7

// Diag frame flag bits (byte 23 "flags2"; gpsValid is OR'ed in at encode)
inline constexpr uint8_t kPowerFlagDegraded  = 0x01;
inline constexpr uint8_t kPowerFlagCritical  = 0x02;
inline constexpr uint8_t kDiagFlagGpsValid   = 0x01;
inline constexpr uint8_t kDiagFlagOtaPending = 0x02;
inline constexpr uint8_t kDiagFlagRecovery   = 0x04;

// OTA status values (diag byte 27)
inline constexpr uint8_t kOtaStatusNone      = 0x00;
inline constexpr uint8_t kOtaStatusStaged    = 0x01; // staged + rebooting into it
inline constexpr uint8_t kOtaStatusNoServer  = 0x02; // unreachable / not configured
inline constexpr uint8_t kOtaStatusDlFailed  = 0x03; // download failed
inline constexpr uint8_t kOtaStatusBadImage  = 0x04; // size/sha mismatch

/// Boot-loop guard: more than this many consecutive unexpected resets
/// triggers recovery mode (long BLE config window + minimal cycle).
inline constexpr uint16_t kMaxUnexpectedResets = 3;

} // namespace minilora
