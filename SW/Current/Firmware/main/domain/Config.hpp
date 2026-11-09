// Device configuration — the domain-side truth.
//
// ABI CONTRACT: `DeviceConfig` is the wire format written by the Android
// BLE configurator inside the CRC envelope (ICD §4). The struct must stay
// packed and field-ordered; new fields are APPENDED and the envelope
// schema bumps (schema 3 = 159-byte body, schema 4 = +5 asset bytes).
// The BLE handler accepts schema 3 AND 4 writes (schema 4 is current).

#pragma once

#include <cstddef>
#include <cstdint>

namespace minilora {

inline constexpr size_t kConfigBodyV3Size = 159;   // v1 layout + cfgZigbee
inline constexpr size_t kConfigBodyV4Size = 164;   // v3 body + 5 asset bytes
inline constexpr size_t kConfigSize = 189;         // v5 body + 25 P2P bytes (§10)

// Config write envelopes (see SW/Current/ICD.md §4):
// 'C' | schema | body | CRC16-CCITT(body) LE
//   schema 3: 159-byte body (asset fields default) — accepted
//   schema 4: 164-byte body with asset config — accepted
//   schema 5: 189-byte body with the P2P fields — CURRENT
inline constexpr size_t kConfigWriteV3Size = 163;  // 1 + 1 + 159 + 2
inline constexpr size_t kConfigWriteV4Size = 168;  // 1 + 1 + 164 + 2
inline constexpr size_t kConfigWriteV5Size = 193;  // 1 + 1 + 189 + 2
inline constexpr uint8_t kConfigWriteMagic = 'C';
inline constexpr uint8_t kConfigWriteMaxSize = kConfigWriteV5Size;

struct __attribute__((packed)) DeviceConfig {
    // ---- v1 ABI (150 bytes) — DO NOT REORDER ----
    char     globalName[32];
    char     wifiSSID[32];
    char     wifiPassword[32];
    uint32_t wifiServerIP;     // octets: o0 = LSB
    uint16_t wifiServerPort;
    uint64_t joinEUI;
    uint64_t devEUI;
    uint8_t  appKey[16];
    uint8_t  nwkKey[16];
    uint8_t  cfgLoRaWAN;
    uint8_t  cfgWiFi;
    uint8_t  cfgGNSS;
    uint8_t  cfgBME280;
    uint8_t  cfgOPT30;
    uint8_t  cfgLIS3DH;
    uint8_t  cfgBLE;
    uint8_t  cfgRange;         // index into kSleepTableMs
    // ---- v2 addition ----
    uint8_t  cfgZigbee;
    // ---- v4 addition (asset mode / P5) ----
    uint8_t  cfgAssetMode;        // wake on LIS3DH motion (INT1 -> EXT1)
    uint8_t  cfgMotionSens;       // 1=low(128 mg) 2=med(256 mg) 3=high(512 mg)
    uint8_t  cfgAssetHeartbeatH;  // hours between asset heartbeats (0 = 24)
    uint8_t  cfgBurstIntervalMin; // sleep after a motion burst (0 = 5)
    uint8_t  cfgBurstMaxPerDay;   // motion-GPS bursts per day (0 = 20)
    // ---- v5 addition (P2P star + trek, ICD §10) ----
    uint8_t  cfgLoRaMode;         // 0 = LoRaWAN (default), 1 = P2P star,
                                  // 2 = trek (two-device bilateral)
    uint8_t  cfgP2PRole;          // 0 = leaf, 1 = gateway
    uint8_t  cfgP2PProfile;       // 0 = long-range SF12, 1 = SF10, 2 = SF7
    uint8_t  cfgP2PRxAfterTx;     // 1 = open the 2 s command window (default)
    uint32_t cfgP2PShortId;       // 0 = derive from the DevEUI's last 4 B
    uint8_t  cfgMeshKey[16];      // mesh PSK — all-zero keeps P2P disabled
    uint8_t  cfgP2PTxPower;       // P2P TX power dBm (0 = 14; max 22 on the
                                  // 869.525 sub-band — see §10.2)
};
static_assert(sizeof(DeviceConfig) == kConfigSize, "BLE ABI broken");

/// Build-time fallbacks (filled from Kconfig by the app layer); also the NVS
/// factory reset content.
struct ConfigDefaults {
    const char *globalName;
    const char *wifiSSID;
    const char *wifiPassword;
    const char *wifiServerIP; // dotted quad
    uint16_t    wifiServerPort;
    uint64_t    joinEUI;
    uint64_t    devEUI;
    const uint8_t *appKey;    // 16 bytes
    const uint8_t *nwkKey;    // 16 bytes
    bool cfgLoRaWAN, cfgWiFi, cfgGNSS, cfgBME280, cfgOPT30, cfgLIS3DH, cfgBLE,
        cfgZigbee;
    uint8_t cfgRange;
    // Asset mode (P5) — defaults applied when the field is absent
    bool cfgAssetMode;
    uint8_t cfgMotionSens;        // 1..3 (0 = med)
    uint8_t cfgAssetHeartbeatH;   // 0 = 24
    uint8_t cfgBurstIntervalMin;  // 0 = 5
    uint8_t cfgBurstMaxPerDay;    // 0 = 20
    // P2P star / trek (§10) — defaults applied when the field is absent
    uint8_t cfgLoRaMode;          // 0 = LoRaWAN (default), 1 = star, 2 = trek
    bool cfgP2PGateway;           // p2p_role = gateway (star only)
    uint8_t cfgP2PProfile;        // 0 = long-range
    bool cfgP2PRxAfterTx;         // default true
    uint32_t cfgP2PShortId;       // 0 = derive from DevEUI
    const uint8_t *meshKey;       // nullptr/zeroed = P2P disabled
    uint8_t cfgP2PTxPower;        // 0 = 14 dBm default (max 22, §10.2)
};

/// Fill `cfg` with `d` (including parsing the dotted-quad IP).
void makeDefaults(const ConfigDefaults &d, DeviceConfig &cfg);

/// Serialize with a 1-byte schema version prefix (blob stored in NVS).
/// `out` must hold kConfigSize + 3 bytes (schema byte + struct + CRC16).
/// Returns bytes written.
size_t serializeConfig(const DeviceConfig &cfg, uint8_t *out);

/// Parse a blob written by serializeConfig (any schema version).
/// Returns true if `len` and the version byte are valid.
bool deserializeConfig(const uint8_t *in, size_t len, DeviceConfig &cfg);

/// CRC16-CCITT (poly 0x1021, init 0xFFFF), the ICD §4 checksum.
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

/// Parse ANY accepted config write (legacy 158, v2 159, v3 envelope 163).
/// Returns false (and leaves `cfg` untouched) on unknown size, bad
/// magic/version, or CRC mismatch — a corrupted write can no longer
/// misconfigure keys.
bool parseConfigWrite(const uint8_t *in, size_t len, DeviceConfig &cfg);

} // namespace minilora
