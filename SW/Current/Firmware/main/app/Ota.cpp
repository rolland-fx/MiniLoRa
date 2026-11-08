#include "Ota.hpp"

#include <cJSON.h>
#include <mbedtls/sha256.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <strings.h>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "../transports/wifi/WifiTransport.hpp"

namespace minilora {

namespace {
const char *TAG = "ota";

// device identity for the check endpoint: DevEUI, 16 hex chars
void devEuiHex(uint64_t devEUI, char out[17])
{
    snprintf(out, 17, "%016llX",
             static_cast<unsigned long long>(devEUI));
}

bool serverUrl(uint32_t ip, uint16_t port, const char *path, char *out,
               size_t outLen)
{
    uint8_t o[4];
    for (int i = 0; i < 4; ++i) {
        o[i] = static_cast<uint8_t>((ip >> (8 * i)) & 0xFF);
    }
    snprintf(out, outLen, "http://%u.%u.%u.%u:%u%s", o[0], o[1], o[2], o[3],
             port, path);
    return true;
}

/// Fetch the check endpoint; fills update=true + path/size/sha256.
bool checkServer(uint32_t ip, uint16_t port, const char *deviceId,
                 const char *currentFw, char *path, size_t pathLen,
                 size_t *sizeOut, char *shaHexOut)
{
    char url[160];
    char query[64];
    snprintf(query, sizeof(query), "/ota/check?device=%s&current=%s",
             deviceId, currentFw);
    serverUrl(ip, port, query, url, sizeof(url));

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8'000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        char buf[512] = {};
        int n = 0, r;
        while (n < static_cast<int>(sizeof(buf)) - 1 &&
               (r = esp_http_client_read(client, buf + n, 1)) > 0) {
            n += r;
        }
        if (status == 200) {
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                const cJSON *upd = cJSON_GetObjectItem(root, "update");
                if (cJSON_IsBool(upd) && cJSON_IsTrue(upd)) {
                    const cJSON *p = cJSON_GetObjectItem(root, "path");
                    const cJSON *s = cJSON_GetObjectItem(root, "size");
                    const cJSON *h = cJSON_GetObjectItem(root, "sha256");
                    if (cJSON_IsString(p) && cJSON_IsNumber(s) &&
                        cJSON_IsString(h)) {
                        snprintf(path, pathLen, "%s", p->valuestring);
                        *sizeOut = static_cast<size_t>(s->valueint);
                        snprintf(shaHexOut, 65, "%s", h->valuestring);
                        ok = true;
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "bad check response: %.80s", buf);
            }
        } else if (status != 404) {
            ESP_LOGW(TAG, "check http %d", status);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}
} // namespace

bool Ota::apply(const DeviceConfig &cfg, uint8_t *statusOut)
{
    const auto finish = [&](bool ok, uint8_t status) {
        if (statusOut) {
            *statusOut = status;
        }
        return ok;
    };
    if (cfg.wifiSSID[0] == '\0' || cfg.wifiServerIP == 0) {
        return finish(false, kOtaStatusNoServer); // no server configured
    }
    char deviceId[17];
    devEuiHex(cfg.devEUI, deviceId);
    char currentFw[16];
    snprintf(currentFw, sizeof(currentFw), "%s", CONFIG_MINILORA_FW_VERSION);

    if (!WifiTransport::start(cfg.wifiSSID, cfg.wifiPassword, 10'000)) {
        return finish(false, kOtaStatusNoServer);
    }

    char path[128] = {};
    size_t imageSize = 0;
    char shaHex[65] = {};
    const bool hasUpdate = checkServer(cfg.wifiServerIP, cfg.wifiServerPort,
                                       deviceId, currentFw, path,
                                       sizeof(path), &imageSize, shaHex);
    if (!hasUpdate) {
        ESP_LOGI(TAG, "no update for %s (current %s)", deviceId, currentFw);
        WifiTransport::stop();
        return finish(false, kOtaStatusNone); // up to date is not a failure
    }
    ESP_LOGI(TAG, "update available: %s (%u bytes, sha %s)", path,
             static_cast<unsigned>(imageSize), shaHex);

    // ---- stage into the passive slot ----
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target =
        esp_ota_get_next_update_partition(running);
    if (!target) {
        ESP_LOGE(TAG, "no OTA slot available");
        WifiTransport::stop();
        return false;
    }
    esp_ota_handle_t handle;
    if (esp_ota_begin(target, imageSize, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        WifiTransport::stop();
        return finish(false, kOtaStatusBadImage);
    }

    esp_http_client_config_t httpCfg = {};
    char url[192];
    serverUrl(cfg.wifiServerIP, cfg.wifiServerPort, path, url, sizeof(url));
    httpCfg.url = url;
    httpCfg.timeout_ms = 15'000;
    esp_http_client_handle_t client = esp_http_client_init(&httpCfg);
    if (!client) {
        esp_ota_abort(handle);
        WifiTransport::stop();
        return finish(false, kOtaStatusDlFailed);
    }

    bool verified = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            static char buf[4096];
            size_t written = 0;
            int r;
            bool failed = false;
            while (written < imageSize &&
                   (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                if (esp_ota_write(handle, buf, r) != ESP_OK ||
                    mbedtls_sha256_update(&sha,
                                          reinterpret_cast<unsigned char *>(buf),
                                          r) != 0) {
                    failed = true;
                    break;
                }
                written += r;
                esp_task_wdt_reset(); // download can take a while
            }
            unsigned char digest[32];
            mbedtls_sha256_finish(&sha, digest);
            char got[65] = {};
            for (int i = 0; i < 32; ++i) {
                snprintf(got + i * 2, 3, "%02x", digest[i]);
            }
            if (!failed && written == imageSize &&
                strcasecmp(got, shaHex) == 0) {
                if (esp_ota_end(handle) == ESP_OK &&
                    esp_ota_set_boot_partition(target) == ESP_OK) {
                    verified = true;
                    ESP_LOGI(TAG, "staged %s (%u bytes, sha ok) — rebooting",
                             path, static_cast<unsigned>(written));
                }
            } else {
                ESP_LOGE(TAG, "image invalid (written=%u expected=%u sha=%s)",
                         static_cast<unsigned>(written),
                         static_cast<unsigned>(imageSize), got);
            }
        } else {
            ESP_LOGW(TAG, "download http %d", status);
        }
    }

    if (!verified) {
        esp_ota_abort(handle); // running image untouched
    }
    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    WifiTransport::stop();
    return finish(verified, verified ? kOtaStatusStaged : kOtaStatusBadImage);
}

} // namespace minilora
