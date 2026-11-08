#include "Config.hpp"

#include "Telemetry.hpp"

#include <cstdio>
#include <cstring>

namespace minilora {

namespace {

inline uint8_t flag(bool b) { return b ? 1 : 0; }

} // namespace

void makeDefaults(const ConfigDefaults &d, DeviceConfig &cfg)
{
    std::memset(&cfg, 0, sizeof(cfg));
    std::strncpy(cfg.globalName, d.globalName, sizeof(cfg.globalName) - 1);
    std::strncpy(cfg.wifiSSID, d.wifiSSID, sizeof(cfg.wifiSSID) - 1);
    std::strncpy(cfg.wifiPassword, d.wifiPassword, sizeof(cfg.wifiPassword) - 1);

    unsigned o[4] = {0, 0, 0, 0};
    std::sscanf(d.wifiServerIP, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]);
    for (int i = 0; i < 4; ++i) {
        cfg.wifiServerIP |= static_cast<uint32_t>(o[i] & 0xFF) << (8 * i);
    }

    cfg.wifiServerPort = d.wifiServerPort;
    cfg.joinEUI = d.joinEUI;
    cfg.devEUI = d.devEUI;
    if (d.appKey) {
        std::memcpy(cfg.appKey, d.appKey, sizeof(cfg.appKey));
    }
    if (d.nwkKey) {
        std::memcpy(cfg.nwkKey, d.nwkKey, sizeof(cfg.nwkKey));
    }

    cfg.cfgLoRaWAN = flag(d.cfgLoRaWAN);
    cfg.cfgWiFi = flag(d.cfgWiFi);
    cfg.cfgGNSS = flag(d.cfgGNSS);
    cfg.cfgBME280 = flag(d.cfgBME280);
    cfg.cfgOPT30 = flag(d.cfgOPT30);
    cfg.cfgLIS3DH = flag(d.cfgLIS3DH);
    cfg.cfgBLE = flag(d.cfgBLE);
    cfg.cfgZigbee = flag(d.cfgZigbee);
    cfg.cfgRange = d.cfgRange < kSleepTableLen ? d.cfgRange : 0;
    cfg.cfgAssetMode = flag(d.cfgAssetMode);
    cfg.cfgMotionSens = d.cfgMotionSens ? d.cfgMotionSens : 2;
    cfg.cfgAssetHeartbeatH = d.cfgAssetHeartbeatH;   // 0 = runtime default 24
    cfg.cfgBurstIntervalMin = d.cfgBurstIntervalMin; // 0 = runtime default 5
    cfg.cfgBurstMaxPerDay = d.cfgBurstMaxPerDay;     // 0 = runtime default 20
}

size_t serializeConfig(const DeviceConfig &cfg, uint8_t *out)
{
    out[0] = 3; // NVS schema 3 = v4 struct (asset fields included)
    std::memcpy(out + 1, &cfg, kConfigSize);
    const uint16_t crc = crc16Ccitt(out + 1, kConfigSize);
    out[kConfigSize + 1] = static_cast<uint8_t>(crc & 0xFF);
    out[kConfigSize + 2] = static_cast<uint8_t>(crc >> 8);
    return kConfigSize + 3;
}

bool deserializeConfig(const uint8_t *in, size_t len, DeviceConfig &cfg)
{
    std::memset(&cfg, 0, sizeof(cfg));
    // NVS load path is CRC-protected like the BLE write path (ICD §4): a
    // corrupted or foreign-layout blob (e.g. a v1 struct left in NVS) must
    // fall back to defaults, never parse as garbage flags.
    if (len != kConfigSize + 3 || in[0] != 3) {
        return false;
    }
    const uint16_t crc = static_cast<uint16_t>(in[kConfigSize + 1]) |
                         static_cast<uint16_t>(in[kConfigSize + 2]) << 8;
    if (crc != crc16Ccitt(in + 1, kConfigSize)) {
        return false;
    }
    std::memcpy(&cfg, in + 1, kConfigSize);
    return true;
}

uint16_t crc16Ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool parseConfigWrite(const uint8_t *in, size_t len, DeviceConfig &cfg)
{
    // ICD §4: CRC-protected envelopes are the only accepted writes.
    //   schema 3 = 159-byte body (asset fields default)
    //   schema 4 = 164-byte body (asset config)  — current
    DeviceConfig parsed{};
    size_t bodyLen = 0;
    if (len == kConfigWriteV3Size && in[1] == 3) {
        bodyLen = kConfigBodyV3Size;
    } else if (len == kConfigWriteV4Size && in[1] == 4) {
        bodyLen = kConfigSize;
    } else {
        return false;
    }
    if (in[0] != kConfigWriteMagic) {
        return false;
    }
    const uint8_t *body = in + 2;
    const uint16_t crc =
        static_cast<uint16_t>(in[2 + bodyLen]) |
        static_cast<uint16_t>(in[2 + bodyLen + 1]) << 8;
    if (crc != crc16Ccitt(body, bodyLen)) {
        return false;
    }
    std::memset(&parsed, 0, sizeof(parsed));
    std::memcpy(&parsed, body, bodyLen); // schema 3: asset tail stays zero
    cfg = parsed;
    return true;
}

} // namespace minilora
