#include "Lis3dh.hpp"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace minilora {

namespace {
const char *TAG = "lis3dh";
constexpr uint8_t REG_WHO_AM_I = 0x0F;
constexpr uint8_t REG_CTRL1 = 0x20;
constexpr uint8_t REG_CTRL4 = 0x23;
constexpr uint8_t REG_CTRL5 = 0x24;
constexpr uint8_t REG_CTRL6 = 0x25;
constexpr uint8_t REG_CTRL3 = 0x22;
constexpr uint8_t REG_STATUS = 0x27; // bit3 ZYXDA = X/Y/Z data available
constexpr uint8_t REG_INT1_CFG = 0x30;
constexpr uint8_t REG_INT1_SRC = 0x31; // read to clear the latched event
constexpr uint8_t REG_INT1_THS = 0x32;
constexpr uint8_t REG_INT1_DUR = 0x33;
constexpr uint8_t REG_OUT_X_L = 0x28; // set bit7 for auto-increment
} // namespace

bool Lis3dh::writeReg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev_, buf, 2, 100) == ESP_OK;
}

bool Lis3dh::init()
{
    uint8_t id = 0;
    const uint8_t r = REG_WHO_AM_I;
    if (i2c_master_transmit_receive(dev_, &r, 1, &id, 1, 100) != ESP_OK || id != 0x33) {
        ESP_LOGE(TAG, "probe failed (id=0x%02x)", id);
        return false;
    }
    // CTRL1: ODR=400Hz (0111), normal mode, XYZ enabled → 0x77
    // CTRL4: FS=01 (±4 g), high-resolution off (v1 parity)
    // CTRL5: D4D_INT1 = open-drain INT1 — REQUIRED on this board: INT1 shares
    //        the PPS_INT net with the push-pull GPS PPS output (HW netlist);
    //        push-pull INT1 would contend with PPS when both are active.
    if (!writeReg(REG_CTRL1, 0x77) || !writeReg(REG_CTRL4, 0x10) ||
        !writeReg(REG_CTRL5, 0x08)) {
        return false;
    }
    // Bench finding 2026-11 (HW doc §5.3 item 1): reads right after the
    // ODR enable returned a CONSTANT 133/133/133 across cycles — the
    // first conversion is not yet in the OUT registers (the datasheet's
    // first-sample latency is 1/ODR + startup; v1 waited 25 ms here).
    // Settle before the caller's first read; read() additionally gates
    // each sample on STATUS.ZYXDA.
    vTaskDelay(pdMS_TO_TICKS(25));
    return true;
}

bool Lis3dh::read(int16_t out[3])
{
    // Wait for a fresh X/Y/Z sample (ZYXDA) instead of blindly reading
    // whatever is in OUT — a not-ready read is how the constant-133
    // artefact happened. Cap: 10 × 2 ms = 20 ms (400 Hz produces a new
    // sample every 2.5 ms, so a healthy sensor answers on the first
    // poll; the cap only bites if the part is genuinely stalled, which
    // then counts as a sensorReadFailure — honest telemetry).
    const uint8_t rs = REG_STATUS;
    bool ready = false;
    for (int i = 0; i < 10; ++i) {
        uint8_t st = 0;
        if (i2c_master_transmit_receive(dev_, &rs, 1, &st, 1, 100) == ESP_OK &&
            (st & 0x08)) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!ready) {
        ESP_LOGW(TAG, "no data-ready within 20 ms");
        return false;
    }
    const uint8_t reg = REG_OUT_X_L | 0x80;
    uint8_t buf[6];
    if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 6, 100) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<int16_t>((buf[2 * i + 1] << 8) | buf[2 * i]) >> 4;
    }
    return true;
}

void Lis3dh::powerDown()
{
    writeReg(REG_CTRL1, 0x07); // ODR=0, keep axes enabled per v1
}

bool Lis3dh::configureMotionWake(int sens)
{
    static const uint8_t kThsBySens[4] = {0, 8, 16, 32}; // 128/256/512 mg
    const uint8_t ths = kThsBySens[sens > 3 ? 3 : (sens < 0 ? 0 : sens)];
    // INT1_SRC read clears any latched event from the awake phase.
    uint8_t src = 0;
    const uint8_t r = REG_INT1_SRC;
    (void)i2c_master_transmit_receive(dev_, &r, 1, &src, 1, 100);
    // CTRL1: ODR=10 Hz, low-power bit, XYZ enabled → 0x1F (~4 µA)
    // CTRL3: I1_IA1 — AOI1 interrupt routed to INT1
    // CTRL6: H_LACTIVE — INT pad active-LOW (shares EXT1 ANY_LOW with
    //        the button; the pad is open-drain per CTRL5 D4D_INT1)
    // INT1_CFG: OR combo, XHIE|YHIE|ZHIE — event on any axis above THS
    if (!writeReg(REG_CTRL1, 0x1F) || !writeReg(REG_CTRL3, 0x40) ||
        !writeReg(REG_CTRL6, 0x02) || !writeReg(REG_INT1_THS, ths) ||
        !writeReg(REG_INT1_DUR, 0x02) || !writeReg(REG_INT1_CFG, 0x2A)) {
        return false;
    }
    return true;
}

} // namespace minilora
