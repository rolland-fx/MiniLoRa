// PA1010D GNSS driver — NMEA over I2C (0x10), parsed with vendored minmea.
// Replicates v1 power control: PMTK220,200 (mid-power) on wake,
// PMTK225,4 (deep standby) before sleep, plus a hardware wake pin.

#pragma once

#include <cstdint>
#include <driver/i2c_master.h>
#include <soc/gpio_num.h>

namespace minilora {

struct GpsFix {
    double latDeg;
    double lonDeg;
    double altM;
    uint8_t sats;
    bool timeValid;
    int year;
    int month;
    int day;
    int hour;
    int min;
    int sec;
};

class Pa1010d {
public:
    explicit Pa1010d(i2c_master_dev_handle_t dev) : dev_(dev) {}

    bool init(gpio_num_t wakePin);
    void wake(bool on);

    /// Pump available NMEA bytes into the parser. Returns true if a fresh
    /// valid fix is available after the call.
    bool poll(GpsFix &out);

    /// PMTK225,4 — deep standby (call before cutting power/deep sleep).
    void deepStandby();

#if CONFIG_MINILORA_GPS_BENCH
    /// Bench-mode counters (see MINILORA_GPS_BENCH): what the module is
    /// actually emitting while the window runs.
    struct Stats {
        uint32_t sentences = 0;   // complete NMEA lines pumped
        uint32_t badChecksum = 0; // $-lines failing checksum verification
        uint32_t gga = 0;
        uint32_t rmc = 0;
    };
    const Stats &stats() const { return stats_; }
#endif

    const GpsFix &lastFix() const { return fix_; }
    bool hasFix() const { return hasFix_; }

private:
    bool sendPmtk(const char *cmd);
    bool pumpBytes();

    i2c_master_dev_handle_t dev_;
    gpio_num_t wakePin_ = GPIO_NUM_NC;
    GpsFix fix_{};
    bool hasFix_ = false;
    char line_[128];
    size_t lineLen_ = 0;
#if CONFIG_MINILORA_GPS_BENCH
    Stats stats_{};
#endif
};

} // namespace minilora
