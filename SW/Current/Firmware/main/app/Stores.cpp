#include "Stores.hpp"

#include <cstring>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace minilora {

namespace {
const char *TAG = "store";
constexpr const char *kNsConfig = "config"; // v1 parity
constexpr const char *kKeyConfig = "cfg";   // v1 parity
constexpr const char *kNsDiag = "diag";
constexpr uint8_t kSchema1 = 1; // 158-byte legacy blob
constexpr uint8_t kSchema2 = 2; // 159-byte v2 blob

void ensureNvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}
} // namespace

bool ConfigStore::initAndLoad(DeviceConfig &cfg, const ConfigDefaults &defaults)
{
    ensureNvs();
    makeDefaults(defaults, cfg); // fallback content

    nvs_handle_t h;
    if (nvs_open(kNsConfig, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no config yet — defaults");
        return false;
    }
    size_t len = 0;
    if (nvs_get_blob(h, kKeyConfig, nullptr, &len) != ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "no config blob — defaults");
        return false;
    }
    // Schema-3 blob is kConfigSize + 3 (schema byte + struct + CRC16) —
    // sizing the buffer kConfigSize + 1 rejected every saved blob on load
    // (bench 2026-11-01: writes "succeeded" but every boot fell back to
    // defaults) and overran the stack by 2 bytes on save.
    uint8_t buf[kConfigSize + 3];
    if (len > sizeof(buf)) {
        nvs_close(h);
        ESP_LOGE(TAG, "config blob too large (%u) — defaults", (unsigned)len);
        return false;
    }
    nvs_get_blob(h, kKeyConfig, buf, &len);
    nvs_close(h);

    // Versioned blob (schema 2) only — legacy v1 NVS layouts are gone.
    return deserializeConfig(buf, len, cfg);
}

bool ConfigStore::save(const DeviceConfig &cfg)
{
    nvs_handle_t h;
    if (nvs_open(kNsConfig, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t blob[kConfigSize + 3];
    const size_t n = serializeConfig(cfg, blob);
    const esp_err_t err = nvs_set_blob(h, kKeyConfig, blob, n);
    nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool ConfigStore::applyWrite(const uint8_t *data, size_t len, DeviceConfig &cfg)
{
    // Single parse path (ICD §4): legacy 158 / v2 159 / v3 envelope 163.
    DeviceConfig parsed{};
    if (!parseConfigWrite(data, len, parsed)) {
        return false;
    }
    cfg = parsed;
    return save(cfg);
}

bool DiagStore::onBoot(Diagnostics &diag)
{
    load(diag);
    ++diag.bootCount;

    const esp_reset_reason_t r = esp_reset_reason();
    diag.lastResetReason = static_cast<uint8_t>(r);
    // USB-JTAG resets are host tooling artifacts (DTR/RTS line discipline),
    // not firmware faults — counting them poisons the boot-loop guard into
    // recovery mode on any bench session. Brownout/PWR_GLITCH stay
    // unexpected: those are real power problems worth counting.
    const bool expected = r == ESP_RST_DEEPSLEEP || r == ESP_RST_POWERON ||
                          r == ESP_RST_SW || r == ESP_RST_USB ||
                          r == ESP_RST_JTAG;
    if (expected) {
        diag.unexpectedResets = 0;
    } else {
        ++diag.unexpectedResets;
    }
    save(diag);
    return diag.unexpectedResets > kMaxUnexpectedResets;
}

void DiagStore::onCleanCycle()
{
    Diagnostics d;
    load(d);
    d.unexpectedResets = 0;
    save(d);
}

void DiagStore::load(Diagnostics &diag)
{
    nvs_handle_t h;
    if (nvs_open(kNsDiag, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_get_u16(h, "bootCount", &diag.bootCount);
    nvs_get_u16(h, "unexpected", &diag.unexpectedResets);
    nvs_get_u16(h, "gpsTo", &diag.gpsTimeouts);
    nvs_get_u16(h, "loraJoin", &diag.loraJoinFailures);
    nvs_get_u16(h, "loraSend", &diag.loraSendFailures);
    nvs_get_u16(h, "wifiFail", &diag.wifiFailures);
    nvs_get_u16(h, "zigFail", &diag.zigbeeFailures);
    nvs_get_u16(h, "sensorFail", &diag.sensorReadFailures);
    nvs_get_u8(h, "gpsConsec", &diag.gpsConsecFails);
    nvs_close(h);
}

void DiagStore::save(const Diagnostics &diag)
{
    nvs_handle_t h;
    if (nvs_open(kNsDiag, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u16(h, "bootCount", diag.bootCount);
    nvs_set_u16(h, "unexpected", diag.unexpectedResets);
    nvs_set_u16(h, "gpsTo", diag.gpsTimeouts);
    nvs_set_u16(h, "loraJoin", diag.loraJoinFailures);
    nvs_set_u16(h, "loraSend", diag.loraSendFailures);
    nvs_set_u16(h, "wifiFail", diag.wifiFailures);
    nvs_set_u16(h, "zigFail", diag.zigbeeFailures);
    nvs_set_u16(h, "sensorFail", diag.sensorReadFailures);
    nvs_set_u8(h, "gpsConsec", diag.gpsConsecFails);
    nvs_commit(h);
    nvs_close(h);
}

} // namespace minilora
