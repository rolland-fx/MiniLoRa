// Config blob serialization + legacy Android ABI tests.

#include "testfw.hpp"

#include "domain/Config.hpp"

#include <cstring>

using namespace minilora;

static void test_size_contract()
{
    // The Android BLE configurator writes the v3 envelope over this body.
    CHECK_EQ(sizeof(DeviceConfig), kConfigSize);
    CHECK_EQ(kConfigWriteV3Size, 163);
}

static void test_serialize_deserialize_roundtrip()
{
    DeviceConfig in{};
    std::strncpy(in.globalName, "unit-test", sizeof(in.globalName) - 1);
    std::strncpy(in.wifiSSID, "ssid", sizeof(in.wifiSSID) - 1);
    std::strncpy(in.wifiPassword, "pass", sizeof(in.wifiPassword) - 1);
    in.wifiServerIP = 0x01020304;
    in.wifiServerPort = 5000;
    in.joinEUI = 0x1122334455667788ULL;
    in.devEUI = 0x99AABBCCDDEEFF00ULL;
    for (int i = 0; i < 16; ++i) {
        in.appKey[i] = static_cast<uint8_t>(i);
        in.nwkKey[i] = static_cast<uint8_t>(255 - i);
    }
    in.cfgLoRaWAN = 1;
    in.cfgBLE = 1;
    in.cfgRange = 4;
    in.cfgZigbee = 1;

    uint8_t blob[kConfigSize + 3];
    CHECK_EQ(serializeConfig(in, blob), kConfigSize + 3);
    CHECK_EQ(blob[0], 3); // NVS schema 3 = v4 struct + trailing CRC16
    // Blob must be tamper-evident: flipping any body byte breaks the load.
    DeviceConfig out{};
    CHECK(deserializeConfig(blob, sizeof(blob), out));
    CHECK(std::memcmp(&in, &out, sizeof(in)) == 0);
    blob[10] ^= 0x40;
    CHECK(!deserializeConfig(blob, sizeof(blob), out));
    blob[10] ^= 0x40;
    CHECK(deserializeConfig(blob, sizeof(blob), out));
}

static void test_rejects_bad_version_and_length()
{
    DeviceConfig cfg{};
    uint8_t blob[kConfigSize + 3] = {};
    blob[0] = 99;
    CHECK(!deserializeConfig(blob, sizeof(blob), cfg));
    CHECK(!deserializeConfig(blob, kConfigSize, cfg));
    CHECK(!deserializeConfig(nullptr, 0, cfg));
    // Zeroed body: right length/schema but the CRC cannot match.
    blob[0] = 3;
    CHECK(!deserializeConfig(blob, sizeof(blob), cfg));
}

static void test_v4_envelope_with_asset_fields()
{
    DeviceConfig in{};
    std::strncpy(in.globalName, "asset", sizeof(in.globalName) - 1);
    in.cfgAssetMode = 1;
    in.cfgMotionSens = 3;
    in.cfgAssetHeartbeatH = 12;
    in.cfgBurstIntervalMin = 2;
    in.cfgBurstMaxPerDay = 10;

    uint8_t env[kConfigWriteV4Size] = {};
    env[0] = kConfigWriteMagic;
    env[1] = 4;
    std::memcpy(env + 2, &in, kConfigSize);
    const uint16_t crc = crc16Ccitt(env + 2, kConfigSize);
    env[kConfigWriteV4Size - 2] = crc & 0xFF;
    env[kConfigWriteV4Size - 1] = crc >> 8;

    DeviceConfig out{};
    CHECK(parseConfigWrite(env, sizeof(env), out));
    CHECK_EQ(std::strcmp(out.globalName, "asset"), 0);
    CHECK_EQ(static_cast<int>(out.cfgAssetMode), 1);
    CHECK_EQ(static_cast<int>(out.cfgMotionSens), 3);
    CHECK_EQ(static_cast<int>(out.cfgAssetHeartbeatH), 12);
    CHECK_EQ(static_cast<int>(out.cfgBurstIntervalMin), 2);
    CHECK_EQ(static_cast<int>(out.cfgBurstMaxPerDay), 10);

    // Schema-3 envelope still parses: the asset tail zero-fills.
    uint8_t env3[kConfigWriteV3Size] = {};
    env3[0] = kConfigWriteMagic;
    env3[1] = 3;
    const uint16_t crc3 = crc16Ccitt(env3 + 2, kConfigBodyV3Size);
    env3[kConfigWriteV3Size - 2] = crc3 & 0xFF;
    env3[kConfigWriteV3Size - 1] = crc3 >> 8;
    DeviceConfig out3{};
    CHECK(parseConfigWrite(env3, sizeof(env3), out3));
    CHECK_EQ(static_cast<int>(out3.cfgAssetMode), 0);
}

static void test_legacy_blobs_are_rejected()
{
    // FW >= 2.4.0: the CRC envelope is the only accepted write. Legacy
    // 158/159-byte blobs (and the schema-1 NVS path) are gone.
    DeviceConfig out{};
    uint8_t legacy[158] = {};
    CHECK(!parseConfigWrite(legacy, sizeof(legacy), out));
    uint8_t v2body[159] = {};
    CHECK(!parseConfigWrite(v2body, sizeof(v2body), out));
    uint8_t blob[159] = {};
    blob[0] = 1; // old NVS schema-1 prefix
    CHECK(!deserializeConfig(blob, sizeof(blob), out));
}

static void test_make_defaults_parses_ip()
{
    const uint8_t appKey[16] = {1};
    ConfigDefaults d{
        .globalName = "MiniLoRa-Test",
        .wifiSSID = "MyWifi",
        .wifiPassword = "secret",
        .wifiServerIP = "192.168.1.10",
        .wifiServerPort = 5000,
        .joinEUI = 0xAABBCCDD11223344ULL,
        .devEUI = 0x70B3D57ED0074E00ULL,
        .appKey = appKey,
        .nwkKey = nullptr,
        .cfgLoRaWAN = true, .cfgWiFi = false, .cfgGNSS = true,
        .cfgBME280 = true, .cfgOPT30 = true, .cfgLIS3DH = true,
        .cfgBLE = true, .cfgZigbee = false,
        .cfgRange = 4,
        .cfgAssetMode = false, .cfgMotionSens = 2,
        .cfgAssetHeartbeatH = 24, .cfgBurstIntervalMin = 5,
        .cfgBurstMaxPerDay = 20,
    };
    DeviceConfig cfg{};
    makeDefaults(d, cfg);
    // 192.168.1.10 little-endian octets
    CHECK_EQ(cfg.wifiServerIP, 0x0A01A8C0u);
    CHECK_EQ(cfg.wifiServerPort, 5000);
    CHECK_EQ(std::strcmp(cfg.globalName, "MiniLoRa-Test"), 0);
    CHECK_EQ(static_cast<int>(cfg.cfgGNSS), 1);
    CHECK_EQ(static_cast<int>(cfg.cfgWiFi), 0);
}

static void test_crc16_ccitt()
{
    // Known-answer: CRC-CCITT (0xFFFF init, poly 0x1021) of "123456789"
    // is 0x29B1 (standard reference value).
    const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc16Ccitt(msg, sizeof(msg)), 0x29B1);
}

static void test_v3_envelope_roundtrip()
{
    DeviceConfig in{};
    std::strncpy(in.globalName, "env-test", sizeof(in.globalName) - 1);
    in.cfgZigbee = 1;
    in.wifiServerPort = 5123;

    uint8_t env[kConfigWriteV3Size] = {};
    env[0] = kConfigWriteMagic;
    env[1] = 3; // schema 3 (159-byte body)
    std::memcpy(env + 2, &in, kConfigBodyV3Size);
    const uint16_t crc = crc16Ccitt(env + 2, kConfigBodyV3Size);
    env[kConfigWriteV3Size - 2] = crc & 0xFF;
    env[kConfigWriteV3Size - 1] = crc >> 8;

    DeviceConfig out{};
    CHECK_EQ(parseConfigWrite(env, sizeof(env), out), true);
    CHECK_EQ(std::strncmp(out.globalName, "env-test",
                          sizeof(out.globalName)),
             0);
    CHECK_EQ(static_cast<int>(out.cfgZigbee), 1);
    CHECK_EQ(out.wifiServerPort, 5123);
}

static void test_v3_envelope_rejects_corruption()
{
    uint8_t env[kConfigWriteV3Size] = {};
    env[0] = kConfigWriteMagic;
    env[1] = 3; // schema 3 (159-byte body)
    const uint16_t crc = crc16Ccitt(env + 2, kConfigSize);
    env[kConfigWriteV3Size - 2] = crc & 0xFF;
    env[kConfigWriteV3Size - 1] = crc >> 8;

    DeviceConfig out{};
    // Corrupted body -> CRC mismatch -> reject, cfg untouched.
    env[10] ^= 0xFF;
    CHECK_EQ(parseConfigWrite(env, sizeof(env), out), false);
    env[10] = 0; // restore body, corrupt CRC instead
    env[kConfigWriteV3Size - 1] ^= 0xFF;
    CHECK_EQ(parseConfigWrite(env, sizeof(env), out), false);
    // Wrong magic / version
    env[kConfigWriteV3Size - 1] ^= 0xFF; // CRC valid again
    env[0] = 'X';
    CHECK_EQ(parseConfigWrite(env, sizeof(env), out), false);
    env[0] = kConfigWriteMagic;
    env[1] = 9;
    CHECK_EQ(parseConfigWrite(env, sizeof(env), out), false);
}

void run_config_tests()
{
    RUN(test_size_contract);
    RUN(test_serialize_deserialize_roundtrip);
    RUN(test_rejects_bad_version_and_length);
    RUN(test_legacy_blobs_are_rejected);
    RUN(test_make_defaults_parses_ip);
    RUN(test_crc16_ccitt);
    RUN(test_v3_envelope_roundtrip);
    RUN(test_v3_envelope_rejects_corruption);
    RUN(test_v4_envelope_with_asset_fields);
}
