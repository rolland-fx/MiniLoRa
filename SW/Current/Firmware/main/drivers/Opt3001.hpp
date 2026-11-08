// OPT3001 luminosity driver — replicates v1 register usage (addr 0x47).
// Continuous conversion, 800 ms integration, auto full-scale; shutdown after read.

#pragma once

#include <driver/i2c_master.h>

namespace minilora {

class Opt3001 {
public:
    explicit Opt3001(i2c_master_dev_handle_t dev) : dev_(dev) {}

    bool init();                       // configure continuous mode
    /// Blocks ~120 ms per call (conversion time); returns lux.
    bool read(float &lux);
    void shutdown();

private:
    bool writeConfig(uint16_t cfg);
    i2c_master_dev_handle_t dev_;
};

} // namespace minilora
