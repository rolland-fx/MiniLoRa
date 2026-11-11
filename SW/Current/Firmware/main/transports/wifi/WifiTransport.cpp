#include "WifiTransport.hpp"

#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "../../domain/Config.hpp"

namespace minilora {

namespace {
const char *TAG = "wifi";

EventGroupHandle_t g_events = nullptr;
constexpr int kBitConnected = BIT0;

void onWifiEvent(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_events, kBitConnected);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_events, kBitConnected);
    }
}

void ipToBytes(uint32_t ip, uint8_t out[4])
{
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>((ip >> (8 * i)) & 0xFF);
    }
}
} // namespace

bool WifiTransport::start(const char *ssid, const char *password,
                          uint32_t timeoutMs)
{
    if (!g_events) {
        g_events = xEventGroupCreate();
    }
    xEventGroupClearBits(g_events, kBitConnected);

    static bool netifReady = false;
    if (!netifReady) {
        esp_netif_init();
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &onWifiEvent, nullptr));
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        netifReady = true;
    }

    // NVS must be initialized before esp_wifi (App::cycle guarantees it)
    wifi_config_t wifiCfg = {};
    strncpy(reinterpret_cast<char *>(wifiCfg.sta.ssid), ssid,
            sizeof(wifiCfg.sta.ssid));
    strncpy(reinterpret_cast<char *>(wifiCfg.sta.password), password,
            sizeof(wifiCfg.sta.password));
    // An empty password = open network (bench/campus/demo SSIDs): the old
    // WPA2-only threshold made every open SSID time out with no hint why.
    wifiCfg.sta.threshold.authmode = (password[0] == '\0')
                                         ? WIFI_AUTH_OPEN
                                         : WIFI_AUTH_WPA_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wifiCfg);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // LAN-range TX power (~13 dBm instead of the 20 dBm default): the
    // server is typically in-building (< 30 m), and the default pulls
    // ~240 mA TX spikes during every push/OTA. Halves the WiFi phase's
    // average current; range is not a concern on this path.
    esp_wifi_set_max_tx_power(52);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    while (xTaskGetTickCount() < deadline) {
        if (xEventGroupGetBits(g_events) & kBitConnected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "connect timeout");
    esp_wifi_stop();
    return false;
}

void WifiTransport::stop()
{
    esp_wifi_stop();
}

bool WifiTransport::postPayload(const uint8_t *payload, size_t len,
                                const char *ssid, const char *password,
                                uint32_t ip, uint16_t port,
                                const char *deviceName, uint32_t timeoutMs,
                                const char *path)
{
    uint8_t octets[4];
    ipToBytes(ip, octets);
    // 96 B: worst case "http://255.255.255.255:65535/data?device=" (42) +
    // a 31-char name. The server prefers ?device= over the client IP, so
    // a DHCP re-address doesn't orphan the device's history.
    char url[96];
    if (deviceName && deviceName[0] != '\0') {
        snprintf(url, sizeof(url),
                 "http://%u.%u.%u.%u:%u%s?device=%s", octets[0],
                 octets[1], octets[2], octets[3], port, path, deviceName);
    } else {
        snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%u%s", octets[0],
                 octets[1], octets[2], octets[3], port, path);
    }

    if (!start(ssid, password, timeoutMs)) {
        return false;
    }

    esp_http_client_config_t httpCfg = {};
    httpCfg.url = url;
    httpCfg.method = HTTP_METHOD_POST;
    httpCfg.timeout_ms = 5'000;
    esp_http_client_handle_t client = esp_http_client_init(&httpCfg);
    bool posted = false;
    if (client) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/octet-stream");
        // Write the body explicitly: perform() alone sends an EMPTY post.
        if (esp_http_client_open(client, static_cast<int>(len)) == ESP_OK) {
            int written = 0;
            while (written < static_cast<int>(len)) {
                int w = esp_http_client_write(
                    client, reinterpret_cast<const char *>(payload + written),
                    static_cast<int>(len - written));
                if (w <= 0) {
                    break;
                }
                written += w;
            }
            esp_http_client_fetch_headers(client);
            const int status = esp_http_client_get_status_code(client);
            char discard[64];
            while (esp_http_client_read(client, discard, sizeof(discard)) > 0) {
            }
            posted = written == static_cast<int>(len) &&
                     status >= 200 && status < 300;
            if (!posted) {
                ESP_LOGW(TAG, "wifi post: written=%d status=%d", written,
                         status);
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    esp_wifi_stop();
    return posted;
}

} // namespace minilora
