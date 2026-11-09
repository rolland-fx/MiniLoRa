#include "P2pRadio.hpp"

#include <driver/gpio.h>
#include <esp_log.h>

#include <RadioLib.h>

#include "../../drivers/Pins.hpp"

namespace minilora {

namespace {
const char *TAG = "p2p";

EspHal *hal = nullptr;
SX1262 *radio = nullptr;

void raiseRfSwitch()
{
    gpio_set_direction(pins::kLoRaRfSw, GPIO_MODE_OUTPUT);
    gpio_hold_dis(pins::kLoRaRfSw);
    gpio_set_level(pins::kLoRaRfSw, 1);
}

void parkPins()
{
    // Park the RF switch low (radio off — RX path disabled, DIO2 idle)
    gpio_set_level(pins::kLoRaRfSw, 0);
    gpio_reset_pin(pins::kLoRaRfSw);
    // Leave the radio bus low-power (v1 prepareSleep pattern)
    gpio_reset_pin(pins::kSX1262Cs);
    gpio_reset_pin(pins::kSX1262Sck);
    gpio_reset_pin(pins::kSX1262Mosi);
    gpio_reset_pin(pins::kSX1262Miso);
    gpio_reset_pin(pins::kSX1262Dio1);
}

} // namespace

bool P2pRadio::begin(p2p::AirProfile profile, uint8_t powerDbm)
{
    if (!hal) {
        hal = new EspHal(pins::kSX1262Sck, pins::kSX1262Miso, pins::kSX1262Mosi,
                         SPI2_HOST, 8'000'000);
    }
    if (!radio) {
        radio = new SX1262(new Module(hal, pins::kSX1262Cs, pins::kSX1262Dio1,
                                      pins::kSX1262Reset, pins::kSX1262Busy));
    }

    const p2p::AirParams &ap = p2p::airParams(profile);
    // Sync word 0x12 = private network (ICD §10.2): invisible to LoRaWAN.
    int16_t state = radio->begin(ap.freqMHz, ap.bwKhz, ap.sf, 5, 0x12,
                                 powerDbm, 8);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "radio begin failed: %d", state);
        return false;
    }
    radio->setDio2AsRfSwitch(true);
    raiseRfSwitch();
    profile_ = profile;
    up_ = true;
    ESP_LOGI(TAG, "p2p radio up: %.3f MHz SF%u slot %u ms",
             ap.freqMHz, ap.sf, ap.slotLenMs);
    return true;
}

bool P2pRadio::channelFree()
{
    if (!up_ || !radio) {
        return false;
    }
    return radio->scanChannel() == RADIOLIB_CHANNEL_FREE;
}

bool P2pRadio::send(const uint8_t *data, size_t len)
{
    if (!up_ || !radio || len == 0) {
        return false;
    }
    return radio->transmit(const_cast<uint8_t *>(data), len) ==
           RADIOLIB_ERR_NONE;
}

int P2pRadio::receive(uint8_t *buf, size_t maxLen, uint32_t timeoutMs,
                      float *rssi, float *snr)
{
    if (!up_ || !radio || maxLen == 0) {
        return 0;
    }
    int16_t state = radio->receive(buf, maxLen, timeoutMs);
    if (state != RADIOLIB_ERR_NONE) {
        return 0;
    }
    if (rssi) {
        *rssi = radio->getRSSI();
    }
    if (snr) {
        *snr = radio->getSNR();
    }
    size_t len = radio->getPacketLength();
    return static_cast<int>(len > maxLen ? maxLen : len);
}

void P2pRadio::park()
{
    if (radio) {
        radio->sleep();
    }
    parkPins();
    up_ = false;
}

} // namespace minilora
