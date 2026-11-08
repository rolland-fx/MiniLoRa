#include "ZigbeeTransport.hpp"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"

// zboss_api.h 1.6.8 declares this without an extern "C" guard; the
// prebuilt lib exports the plain C symbol, so redeclare it here.
extern "C" {
typedef int zb_ret_t;
// matches zb_nvram_dataset_types_e in zboss_api.h
typedef enum zb_nvram_dataset_types_c {
    ZB_NVRAM_COMMON_DATA_C = 1,
    ZB_NVRAM_HA_DATA_C = 2,
} zb_nvram_dataset_types_c_t;
zb_ret_t zb_nvram_write_dataset(zb_nvram_dataset_types_c_t t);
}

#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"
#include "zcl/esp_zigbee_zcl_humidity_meas.h"
#include "zcl/esp_zigbee_zcl_illuminance_meas.h"
#include "zcl/esp_zigbee_zcl_pressure_meas.h"
#include "zcl/esp_zigbee_zcl_occupancy_sensing.h"

namespace minilora {

namespace {
const char *TAG = "zigbee";

constexpr uint8_t kEndpoint = 1;
constexpr uint16_t kCoordinatorShort = 0x0000;
constexpr uint8_t kCoordinatorEndpoint = 1;

// Motion detection: LIS3DH counts; delta above this between cycles = motion
constexpr int32_t kMotionDeltaCounts = 400; // ≈ 0.2 g

volatile bool g_joined = false;
volatile bool g_stop = false;
TaskHandle_t g_task = nullptr;

void zigbeeTask(void *)
{
    while (!g_stop) {
        esp_zb_main_loop_iteration();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

void commissioningAlarm(uint8_t mode)
{
    esp_zb_bdb_start_top_level_commissioning(mode);
}

// The stack calls this (declared in esp_zigbee_core.h).
extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
    const uint32_t sig = *signal->p_app_signal;
    const esp_err_t err = signal->esp_err_status;
    void *params = esp_zb_app_signal_get_params(signal->p_app_signal);

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack init; starting commissioning");
        esp_zb_scheduler_alarm(
            reinterpret_cast<esp_zb_callback_t>(commissioningAlarm),
            ESP_ZB_BDB_MODE_INITIALIZATION, 100);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "factory new — steering");
                esp_zb_scheduler_alarm(
                    reinterpret_cast<esp_zb_callback_t>(commissioningAlarm),
                    ESP_ZB_BDB_MODE_NETWORK_STEERING, 100);
            } else {
                ESP_LOGI(TAG, "network restored from NVRAM");
                g_joined = true;
            }
        } else {
            ESP_LOGW(TAG, "device start failed (0x%x), retry", err);
            esp_zb_scheduler_alarm(
                reinterpret_cast<esp_zb_callback_t>(commissioningAlarm),
                ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            g_joined = true;
            ESP_LOGI(TAG, "joined, short addr 0x%04x",
                     esp_zb_get_short_address());
        } else {
            ESP_LOGW(TAG, "steering failed (0x%x)", err);
        }
        break;

    default:
        break;
    }
    (void)params;
}

esp_zb_cluster_list_t *buildEndpoint()
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    // Basic
    esp_zb_basic_cluster_cfg_t basic = {};
    basic.zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE;
    basic.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
    esp_zb_attribute_list_t *basicAttr = esp_zb_basic_cluster_create(&basic);
    esp_zb_cluster_list_add_basic_cluster(list, basicAttr,
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Power config (battery): voltage in 100 mV units set per cycle
    esp_zb_power_config_cluster_cfg_t power = {};
    esp_zb_attribute_list_t *powerAttr =
        esp_zb_power_config_cluster_create(&power);
    esp_zb_cluster_list_add_power_config_cluster(list, powerAttr,
                                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Environmental sensors — ranges per BME280/OPT3001 capability
    esp_zb_temperature_meas_cluster_cfg_t temp = {};
    temp.measured_value = 0;
    temp.min_value = -4000;  // -40.00 °C
    temp.max_value = 8500;   //  85.00 °C
    esp_zb_cluster_list_add_temperature_meas_cluster(
        list, esp_zb_temperature_meas_cluster_create(&temp),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_humidity_meas_cluster_cfg_t hum = {};
    hum.measured_value = 0;
    hum.min_value = 0;
    hum.max_value = 10000;
    esp_zb_cluster_list_add_humidity_meas_cluster(
        list, esp_zb_humidity_meas_cluster_create(&hum),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_pressure_meas_cluster_cfg_t pres = {};
    pres.measured_value = 0;
    // ZCL 4.5.2.2.1.1: MeasuredValue unit is 0.1 kPa = 1 hPa — publish()
    // sends raw hPa, so the bounds must be hPa too (300–1200). The old
    // 30000/12000 came from a 0.1 hPa mix-up and put the reported value
    // (~1013) outside the declared range.
    pres.min_value = 300;   // 300 hPa
    pres.max_value = 1200;  // 1200 hPa
    esp_zb_cluster_list_add_pressure_meas_cluster(
        list, esp_zb_pressure_meas_cluster_create(&pres),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_illuminance_meas_cluster_cfg_t illum = {};
    illum.measured_value = 0;
    illum.min_value = 0;
    illum.max_value = 0xFFFF;
    esp_zb_cluster_list_add_illuminance_meas_cluster(
        list, esp_zb_illuminance_meas_cluster_create(&illum),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Occupancy: motion derived from accel delta (anti-theft / automation)
    esp_zb_occupancy_sensing_cluster_cfg_t occ = {};
    occ.occupancy = 0;
    occ.sensor_type = ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR;
    occ.sensor_type_bitmap = 1; // PIR bit set
    esp_zb_cluster_list_add_occupancy_sensing_cluster(
        list, esp_zb_occupancy_sensing_cluster_create(&occ),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

void setAttr(uint16_t cluster, uint16_t attr, const void *value)
{
    esp_zb_zcl_status_t st = esp_zb_zcl_set_attribute_val(
        kEndpoint, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr,
        const_cast<void *>(value), false);
    if (st != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set attr cluster 0x%04x failed: 0x%x", cluster, st);
    }
}

void report(uint16_t cluster, uint16_t attr)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {};
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = kCoordinatorShort;
    cmd.zcl_basic_cmd.dst_endpoint = kCoordinatorEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = kEndpoint;
    cmd.clusterID = cluster;
    cmd.attributeID = attr;
    esp_zb_zcl_report_attr_cmd_req(&cmd);
}

int32_t accelDelta(const TelemetryData &t, const int16_t prev[3])
{
    int32_t maxDelta = 0;
    for (int i = 0; i < 3; ++i) {
        const int32_t d = std::abs(static_cast<int32_t>(t.acc[i]) - prev[i]);
        if (d > maxDelta) {
            maxDelta = d;
        }
    }
    return maxDelta;
}

} // namespace

bool ZigbeeTransport::startAndJoin(uint32_t timeoutMs)
{
    if (!g_task) {
        g_stop = false;
        esp_zb_cfg_t cfg = {};
        cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ED;
        cfg.install_code_policy = false;
        cfg.nwk_cfg.zed_cfg.ed_timeout =
            ESP_ZB_ED_AGING_TIMEOUT_8192MIN;   // deep-sleep friendly
        cfg.nwk_cfg.zed_cfg.keep_alive = 30'000;

        esp_zb_nvram_erase_at_start(false); // keep NVRAM (default, explicit)
        esp_zb_init(&cfg);
        esp_zb_ep_list_t *epList = esp_zb_ep_list_create();
        esp_zb_endpoint_config_t epCfg = {};
        epCfg.endpoint = kEndpoint;
        epCfg.app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID;
        epCfg.app_device_version = 0;
        esp_zb_ep_list_add_ep(epList, buildEndpoint(), epCfg);
        esp_zb_device_register(epList);
        esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
        esp_zb_start(false);
        xTaskCreate(zigbeeTask, "zigbee", 8192, nullptr, 5, &g_task);
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    while (xTaskGetTickCount() < deadline) {
        if (g_joined) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "join timeout (%u ms)", timeoutMs);
    return false;
}

bool ZigbeeTransport::publish(const TelemetryData &t, const int16_t prevAcc[3])
{
    if (!g_joined) {
        return false;
    }

    const int16_t temp = static_cast<int16_t>(t.tempC * 100);
    const uint16_t hum = static_cast<uint16_t>(t.humPct * 100);
    const int16_t pres = static_cast<int16_t>(t.presDeciHpa / 10); // ZCL: hPa
    const uint16_t lux = t.lux;
    const uint8_t occ = accelDelta(t, prevAcc) > kMotionDeltaCounts ? 1 : 0;
    const uint8_t batVolt = static_cast<uint8_t>(
        (t.batMv / 100 > 255) ? 255 : t.batMv / 100); // 100 mV units
    const uint8_t batPct = static_cast<uint8_t>(   // 0..200 (0.5% steps)
        ((static_cast<uint32_t>(t.batMv) - 2500) * 200) / 1800);

    setAttr(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temp);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &hum);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID, &pres);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
            ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID, &lux);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
            ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, &occ);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &batVolt);
    setAttr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            &batPct);

    vTaskDelay(pdMS_TO_TICKS(100));
    report(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
           ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
    report(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
           ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
    report(ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT,
           ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID);
    report(ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
           ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID);
    report(ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
           ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID);
    report(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
           ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);
    return true;
}

void ZigbeeTransport::persist()
{
    if (!g_task) {
        return;
    }
    // Persist network context + ZCL reporting config to the zb_storage
    // partition; deep sleep will wipe RAM right after.
    zb_nvram_write_dataset(ZB_NVRAM_COMMON_DATA_C);
    zb_nvram_write_dataset(ZB_NVRAM_HA_DATA_C);
    vTaskDelay(pdMS_TO_TICKS(300));
}

} // namespace minilora
