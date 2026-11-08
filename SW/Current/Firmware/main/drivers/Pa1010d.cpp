#include "Pa1010d.hpp"

#include <cstring>
#include <driver/gpio.h>
#include <esp_log.h>

extern "C" {
#include "minmea/minmea.h"
}

namespace minilora {

namespace {
const char *TAG = "pa1010d";

// v1 constants
constexpr const char *CMD_MID_POWER = "$PMTK220,200*2C\r\n";  // 5 Hz fix rate
constexpr const char *CMD_NMEA_OUTPUT =
    "$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28\r\n"; // RMC+GGA 1 Hz
constexpr const char *CMD_DEEP_SLEEP = "$PMTK225,4*2F\r\n";   // deep standby
constexpr uint8_t REG_DATA_STREAM = 0x0A;                     // SparkFun-lib compatible
constexpr size_t kChunk = 192;
constexpr uint8_t kMinSatellites = 7; // v1 GPS_MIN_SATELLITES
} // namespace

bool Pa1010d::sendPmtk(const char *cmd)
{
    return i2c_master_transmit(dev_, reinterpret_cast<const uint8_t *>(cmd),
                               strlen(cmd), 100) == ESP_OK;
}

bool Pa1010d::init(gpio_num_t wakePin)
{
    wakePin_ = wakePin;
    wake(true);
    // Pin the NMEA output to RMC+GGA at 1 Hz: the module's NVM default set
    // was observed emitting ~1 RMC per 5 minutes (bench 2026-11-03), and
    // the RMC carries the date our RTC anchor wants. This also drops the
    // long GSA/GSV sentences that pressure the 128-byte line buffer.
    if (!sendPmtk(CMD_NMEA_OUTPUT)) {
        return false;
    }
    return sendPmtk(CMD_MID_POWER);
}

void Pa1010d::wake(bool on)
{
    if (wakePin_ == GPIO_NUM_NC) {
        return;
    }
    gpio_set_direction(wakePin_, GPIO_MODE_OUTPUT);
    gpio_set_level(wakePin_, on ? 1 : 0);
}

bool Pa1010d::pumpBytes()
{
    const uint8_t reg = REG_DATA_STREAM;
    uint8_t buf[kChunk];
    if (i2c_master_transmit_receive(dev_, &reg, 1, buf, kChunk, 50) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < kChunk; ++i) {
        const char c = static_cast<char>(buf[i]);
        if (c == '$') {
            lineLen_ = 0; // sentence start (also drops stale data)
        }
        if (lineLen_ + 1 >= sizeof(line_)) {
            lineLen_ = 0;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (lineLen_ > 0) {
                line_[lineLen_] = '\0';
                return true; // one sentence ready
            }
            continue;
        }
        line_[lineLen_++] = c;
    }
    return false;
}

bool Pa1010d::poll(GpsFix &out)
{
    bool sentenceReady = false;
    while (pumpBytes()) {
        sentenceReady = true;
        // Checksum ON: a corrupted I2C chunk (interleaved sentence fragment)
        // otherwise parses as a valid RMC with shifted fields — shipped as
        // a position+date frame dated 2068 (2026-11-03). A dropped sentence
        // costs one poll second; a garbage date costs the fix.
        enum minmea_sentence_id id = minmea_sentence_id(line_, true);

#if CONFIG_MINILORA_GPS_BENCH
        ++stats_.sentences;
        if (id == MINMEA_INVALID && line_[0] == '$') {
            ++stats_.badChecksum; // $-line that failed verification
        }
#endif
        if (id == MINMEA_SENTENCE_RMC) {
#if CONFIG_MINILORA_GPS_BENCH
            ++stats_.rmc;
#endif
            struct minmea_sentence_rmc rmc;
            if (minmea_parse_rmc(&rmc, line_) && rmc.valid) {
                fix_.latDeg = minmea_tocoord(&rmc.latitude);
                fix_.lonDeg = minmea_tocoord(&rmc.longitude);
                if (rmc.date.year > 0) {
                    fix_.timeValid = true;
                    fix_.year = rmc.date.year;
                    fix_.month = rmc.date.month;
                    fix_.day = rmc.date.day;
                    fix_.hour = rmc.time.hours;
                    fix_.min = rmc.time.minutes;
                    fix_.sec = rmc.time.seconds;
                }
                hasFix_ = true;
            }
        } else if (id == MINMEA_SENTENCE_GGA) {
#if CONFIG_MINILORA_GPS_BENCH
            ++stats_.gga;
#endif
            struct minmea_sentence_gga gga;
            if (minmea_parse_gga(&gga, line_)) {
                fix_.sats = gga.satellites_tracked;
                fix_.altM = minmea_tofloat(&gga.altitude);
                if (gga.satellites_tracked >= kMinSatellites &&
                    gga.latitude.value != 0) {
                    fix_.latDeg = minmea_tocoord(&gga.latitude);
                    fix_.lonDeg = minmea_tocoord(&gga.longitude);
                    hasFix_ = true;
                }
            }
        }
        lineLen_ = 0;
    }
    out = fix_;
    return sentenceReady && hasFix_;
}

void Pa1010d::deepStandby()
{
    sendPmtk(CMD_DEEP_SLEEP);
}

} // namespace minilora
