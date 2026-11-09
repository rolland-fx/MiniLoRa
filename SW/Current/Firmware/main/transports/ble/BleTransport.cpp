#include "BleTransport.hpp"

#include <cstring>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../drivers/BoardPower.hpp"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace minilora {

namespace {
const char *TAG = "ble";

// 0000FACE-0000-1000-8000-00805F9B34FB / 0000CAFE-… / 0000CAFF-… (v1 UUIDs + v3 status)
constexpr uint8_t kSvcUuid[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00,
                                  0x80, 0x00, 0x10, 0x00, 0x00, 0xCE, 0xFA,
                                  0x00, 0x00};
constexpr uint8_t kChrUuid[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00,
                                  0x80, 0x00, 0x10, 0x00, 0x00, 0xFE, 0xCA,
                                  0x00, 0x00};
constexpr uint8_t kStatusUuid[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
                                     0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                                     0xFF, 0xCA, 0x00, 0x00};

constexpr uint32_t kBeaconIntervalUnits = 1000; // 625 ms (v1 used 10 s — a bug)
constexpr uint16_t kExtAdvHandle = 0;
constexpr uint8_t kAdvHandleCfg = 0;

// shared state
enum class Mode { Idle, Beacon, Config };
volatile Mode g_mode = Mode::Idle;
volatile bool g_synced = false;
volatile bool g_clientInteracted = false;
char g_name[32] = "MiniLoRa";

int configAccessCb(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt,
                   void *arg);
int statusAccessCb(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt,
                   void *arg);

// staging area used by the GATT callback (single-threaded cycle model)
static DeviceConfig g_stagedCfg{};
static BleTransport::ConfigResult g_cfgResult;
static BleTransport::ConfigResult *g_result = &g_cfgResult;
static BleTransport::StatusProvider g_statusProvider = nullptr;

ble_uuid128_t g_svcUuid = BLE_UUID128_INIT(/* filled below */);
ble_uuid128_t g_chrUuid = BLE_UUID128_INIT(/* filled below */);
ble_uuid128_t g_statusUuid = BLE_UUID128_INIT(/* filled below */);

// 16-bit alias of the config service UUID (0xFACE), used in the config
// advertisement so name + UUID fit the legacy 31-byte payload.
const ble_uuid16_t kConfigSvcUuid16 = BLE_UUID16_INIT(0xFACE);

const struct ble_gatt_chr_def g_chrs[] = {
    {.uuid = reinterpret_cast<const ble_uuid_t *>(&g_chrUuid),
     .access_cb = configAccessCb,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
     .min_key_size = 0,
     .val_handle = nullptr},
    {.uuid = reinterpret_cast<const ble_uuid_t *>(&g_statusUuid),
     .access_cb = statusAccessCb,
     .flags = BLE_GATT_CHR_F_READ,
     .min_key_size = 0,
     .val_handle = nullptr},
    {},
};

const struct ble_gatt_svc_def g_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = reinterpret_cast<const ble_uuid_t *>(&g_svcUuid),
     .characteristics = g_chrs},
    {},
};

void buildUuids()
{
    // BLE_UUID128_INIT takes the 16 bytes in little-endian order
    std::memcpy(g_svcUuid.value, kSvcUuid, 16);
    std::memcpy(g_chrUuid.value, kChrUuid, 16);
    std::memcpy(g_statusUuid.value, kStatusUuid, 16);
}

void onSync()
{
    g_synced = true;
}

void hostTask(void *)
{
    nimble_port_run(); // returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

bool startNimble()
{
    buildUuids();
    if (nimble_port_init() != ESP_OK) {
        return false;
    }
    ble_hs_cfg.sync_cb = onSync;
    // GATT services MUST be registered before the host task starts: the
    // ATT database is frozen at host start, and anything added after the
    // sync never appears in the live server (bench: "Config service not
    // found" with a 200 status=0 discovery).
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int grc = ble_gatts_count_cfg(g_services);
    if (grc != 0) {
        ESP_LOGE(TAG, "gatts count_cfg rc=%d", grc);
    }
    grc = ble_gatts_add_svcs(g_services);
    if (grc != 0) {
        ESP_LOGE(TAG, "gatts add_svcs rc=%d", grc);
    }
    ble_svc_gap_device_name_set(g_name);
    nimble_port_freertos_init(hostTask);
    // wait for host sync
    for (int i = 0; i < 100 && !g_synced; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return g_synced;
}

void stopNimble()
{
    nimble_port_stop();
    nimble_port_deinit();
    g_synced = false;
}

// ---- beacon -------------------------------------------------------------

// Configure the shared ext-adv instance and put `ad` on air (coded PHY,
// kBeaconIntervalUnits). Both beacon builders funnel through here.
static void startExtAdvWithData(const uint8_t *ad, size_t n)
{
    ble_gap_ext_adv_params params = {};
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.connectable = 0;
    params.legacy_pdu = 0;               // extended advertising
    params.scannable = 0;
    params.itvl_min = kBeaconIntervalUnits;
    params.itvl_max = kBeaconIntervalUnits;
    params.primary_phy = BLE_HCI_LE_PHY_CODED;
    params.secondary_phy = BLE_HCI_LE_PHY_CODED;
    params.sid = kExtAdvHandle;
    int rc = ble_gap_ext_adv_configure(kExtAdvHandle, &params, nullptr,
                                       nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext adv configure rc=%d", rc);
        return;
    }
    {
        struct os_mbuf *data = os_msys_get_pkthdr(n, 0);
        if (!data || os_mbuf_append(data, ad, n) != 0) {
            ESP_LOGE(TAG, "ext adv mbuf alloc failed");
            return;
        }
        rc = ble_gap_ext_adv_set_data(kExtAdvHandle, data);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ext adv data rc=%d", rc);
        return;
    }
    rc = ble_gap_ext_adv_start(kExtAdvHandle, BLE_HS_FOREVER, 0);
    ESP_LOGI(TAG, "beacon adv rc=%d", rc);
}

// Own beacon — the v1-EXACT byte stream (bench-validated 2026-11-01):
//   [len = 2 + len - 1][0xFF][0xFF][0xFF][payload]
// The phone's parser keys on this exact shape; do not "clean it up".
void startBeaconAdvertising(const uint8_t *payload, size_t len)
{
    uint8_t ad[2 + 2 + 28];
    size_t n = 0;
    ad[n++] = static_cast<uint8_t>(2 + len - 1);
    ad[n++] = 0xFF;                      // manufacturer data
    ad[n++] = 0xFF;                      // company id LSB (test value, v1 parity)
    ad[n++] = 0xFF;                      // company id MSB
    memcpy(ad + n, payload, len);
    n += len;
    startExtAdvWithData(ad, n);
}

// Peer relay (ICD §10.9) — the own-beacon shape with a 0x02 type marker
// before the origin:
//   [len][0xFF][0xFF][0xFF][0x02][origin BE][seq BE][§1 body]
// The length byte is the TRUE structure length (1+2+1+4+4+body), so the
// phone's manufacturer field reads [FF][FF][0x02][origin][seq][body] = 39 B.
void startPeerRelayAdvertising(const uint8_t *body, size_t bodyLen,
                               uint32_t origin, uint32_t seq)
{
    uint8_t ad[4 + 1 + 4 + 4 + 28];
    size_t n = 0;
    ad[n++] = static_cast<uint8_t>(1 + 2 + 1 + 4 + 4 + bodyLen);
    ad[n++] = 0xFF;                      // manufacturer data
    ad[n++] = 0xFF;                      // company id LSB (v1 parity)
    ad[n++] = 0xFF;                      // company id MSB
    ad[n++] = 0x02;                      // peer-relay marker
    for (int i = 0; i < 4; ++i) {
        ad[n++] = static_cast<uint8_t>(origin >> (24 - 8 * i));
    }
    for (int i = 0; i < 4; ++i) {
        ad[n++] = static_cast<uint8_t>(seq >> (24 - 8 * i));
    }
    memcpy(ad + n, body, bodyLen);
    n += bodyLen;
    startExtAdvWithData(ad, n);
}

// ---- config service ------------------------------------------------------

// The legacy ble_gap_adv_* API is a compile-time stub returning
// BLE_HS_ENOTSUP when MYNEWT_VAL(BLE_EXT_ADV) is on (the ESP-IDF default):
// the config window used it and NEVER advertised at all (bench:
// "adv fields rc=8", invisible to every scanner). This mirrors the v1
// NimBLE-Arduino recipe (BLEConfigStart): connectable + scannable LEGACY
// PDU (ADV_IND) on the ext-adv API, visible to every scanner.
static volatile bool g_bleConnected = false;

int configGapEvent(ble_gap_event *event, void *)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        g_bleConnected = true; // LED goes steady: the phone is talking
        ESP_LOGI(TAG, "config: client connected");
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        g_bleConnected = false;
        ESP_LOGI(TAG, "config: client disconnected");
        // NimBLE auto-stops a connectable adv instance when the connection
        // forms; without this the config window stays open (LED blinks) but
        // the radio is silent — the next phone action times out (Android
        // GATT status 147). Instance is still configured; just restart it.
        {
            const int rc = ble_gap_ext_adv_start(kExtAdvHandle, BLE_HS_FOREVER, 0);
            if (rc != 0) {
                ESP_LOGE(TAG, "config adv restart rc=%d", rc);
            }
        }
        return 0;
    default:
        return 0;
    }
}

void startConfigAdvertising()
{
    // AD structure: flags + 16-bit UUID (0xFACE) + complete name ≈ 22 B,
    // fits the 31-byte legacy PDU. (The 128-bit UUID + name = 36 B did
    // not fit anywhere.) 0xFACE is the 16-bit alias of the config service
    // UUID; Android resolves it to the same 128-bit base the GATT server
    // exposes, so the app-side filter needs no change.
    uint8_t ad[31];
    size_t n = 0;
    const uint8_t nameLen =
        static_cast<uint8_t>(strnlen(g_name, sizeof g_name));
    ad[n++] = 2;                                        // len (type + 1)
    ad[n++] = BLE_HS_ADV_TYPE_FLAGS;
    ad[n++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ad[n++] = 3;                                        // len (type + 2)
    ad[n++] = BLE_HS_ADV_TYPE_COMP_UUIDS16;
    ad[n++] = static_cast<uint8_t>(0xFACE & 0xFF);      // 0xFACE, little-endian
    ad[n++] = static_cast<uint8_t>(0xFACE >> 8);
    ad[n++] = static_cast<uint8_t>(nameLen + 1);
    ad[n++] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(ad + n, g_name, nameLen);
    n += nameLen;

    ble_gap_ext_adv_params params = {};
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.connectable = 1;
    params.scannable = 1;
    params.legacy_pdu = 1;               // ADV_IND: visible to all scanners
    params.itvl_min = 160;               // 100 ms
    params.itvl_max = 160;
    params.sid = kExtAdvHandle;
    int rc = ble_gap_ext_adv_configure(kExtAdvHandle, &params, nullptr,
                                       configGapEvent, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "config ext adv configure rc=%d", rc);
        return;
    }
    {
        struct os_mbuf *data = os_msys_get_pkthdr(n, 0);
        if (!data || os_mbuf_append(data, ad, n) != 0) {
            ESP_LOGE(TAG, "config ext adv mbuf alloc failed");
            return;
        }
        rc = ble_gap_ext_adv_set_data(kExtAdvHandle, data);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "config ext adv data rc=%d", rc);
        return;
    }
    rc = ble_gap_ext_adv_start(kExtAdvHandle, BLE_HS_FOREVER, 0);
    ESP_LOGI(TAG, "config adv rc=%d", rc);
}

int configAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // ICD §4: the read returns the schema-5 envelope
        // ('C'|5|body|CRC) so the app can verify the CRC of what it read.
        DeviceConfig cfg = g_stagedCfg;
        uint8_t env[kConfigWriteV5Size];
        env[0] = kConfigWriteMagic;
        env[1] = 5;
        std::memcpy(env + 2, &cfg, kConfigSize);
        const uint16_t crc = crc16Ccitt(env + 2, kConfigSize);
        env[kConfigWriteV5Size - 2] = static_cast<uint8_t>(crc & 0xFF);
        env[kConfigWriteV5Size - 1] = static_cast<uint8_t>(crc >> 8);
        os_mbuf_append(ctxt->om, env, sizeof(env));
        g_clientInteracted = true;
        return 0;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t omLen = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[kConfigWriteMaxSize];
        if (omLen > sizeof(buf)) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        os_mbuf_copydata(ctxt->om, 0, omLen, buf);
        // Envelope only (ICD §4, FW ≥ 2.4.0): bad magic/version/size/CRC
        // -> reject, config stays untouched.
        if (parseConfigWrite(buf, omLen, g_result->cfg)) {
            g_result->written = true;
        } else {
            ESP_LOGW(TAG, "config write rejected (%u bytes)", omLen);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        g_clientInteracted = true;
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

int statusAccessCb(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *)
{
    uint8_t frame[BleTransport::kStatusSize] = {};
    size_t n = 0;
    if (g_statusProvider) {
        n = g_statusProvider(frame);
    }
    if (n == 0) {
        // Schema-1 frame with everything zeroed — still parseable.
        frame[0] = 0x01;
        n = sizeof(frame);
    }
    os_mbuf_append(ctxt->om, frame, n);
    g_clientInteracted = true;
    return 0;
}

} // namespace

void BleTransport::beacon(const uint8_t *payload, size_t len, const char *name,
                          uint32_t durationMs)
{
    if (!payload || len == 0 || len > 28) {
        return;
    }
    strncpy(g_name, name, sizeof(g_name) - 1);
    if (!startNimble()) {
        return;
    }
    startBeaconAdvertising(payload, len);
    // Feed the task WDT while blocking (the main task is subscribed for
    // the whole cycle; long single delays starve it).
    const TickType_t wake = xTaskGetTickCount() + pdMS_TO_TICKS(durationMs);
    while (xTaskGetTickCount() < wake) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ble_gap_ext_adv_stop(kExtAdvHandle);
    stopNimble();
}

void BleTransport::beaconPeer(const uint8_t *body28, size_t len,
                              uint32_t peerOrigin, uint32_t peerSeq,
                              const char *name, uint32_t durationMs)
{
    if (!body28 || len == 0 || len > 28) {
        return;
    }
    strncpy(g_name, name, sizeof(g_name) - 1);
    if (!startNimble()) {
        return;
    }
    // ICD §10.9: [len][FF][FF][FF][0x02][origin BE][seq BE][§1 body] — the
    // 0x02 marker distinguishes it from a plain 28-byte v1 beacon.
    startPeerRelayAdvertising(body28, len, peerOrigin, peerSeq);
    const TickType_t wake = xTaskGetTickCount() + pdMS_TO_TICKS(durationMs);
    while (xTaskGetTickCount() < wake) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ble_gap_ext_adv_stop(kExtAdvHandle);
    stopNimble();
}

BleTransport::ConfigResult BleTransport::runConfigService(
    const DeviceConfig &current, const char *name, uint32_t timeoutMs,
    StatusProvider status)
{
    strncpy(g_name, name, sizeof(g_name) - 1);
    g_cfgResult = ConfigResult{};
    g_stagedCfg = current;
    g_clientInteracted = false;
    g_statusProvider = status;

    if (!startNimble()) {
        return g_cfgResult;
    }
    int rc = ble_gatts_count_cfg(g_services);
    rc |= ble_gatts_add_svcs(g_services);
    if (rc == 0) {
        ble_svc_gap_init(); // idempotent-safe ordering with gatts
        startConfigAdvertising();
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    // LED UX: ~1 Hz blink while advertising, steady ON once the phone is
    // connected. NO early exit on g_clientInteracted: the app's normal
    // flow is Read -> edit -> Write, and a Read fires that flag — the old
    // break closed the window before the Write ever arrived.
    BoardPower::instance().ledBlue(false);
    uint16_t blinkTick = 0;
    while (xTaskGetTickCount() < deadline) {
        // The main task subscribes to the task WDT for the whole cycle;
        // a >5 s window here must keep feeding it or every config window
        // spams watchdog dumps (and masks real starvation).
        esp_task_wdt_reset();
        if (g_result->written) {
            break; // Save landed — caller persists; no need to linger
        }
        BoardPower::instance().ledBlue(g_bleConnected ? true
                                                      : (blinkTick / 5) % 2 == 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        ++blinkTick;
    }
    ble_gap_ext_adv_stop(kExtAdvHandle);
    BoardPower::instance().ledBlue(false);
    stopNimble();
    return g_cfgResult;
}

} // namespace minilora
