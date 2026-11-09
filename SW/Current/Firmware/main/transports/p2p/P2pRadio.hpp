// P2P star radio — the SX1262 in raw mode (no LoRaWAN stack), ICD §10.
// Same construction/parking conventions as LoRaWanTransport (DIO2 as RF
// switch, kLoRaRfSw HIGH for the RX path, pins reset on park) so the two
// transports are interchangeable in the cycle.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../domain/P2pSchedule.hpp"

namespace minilora {

class P2pRadio {
public:
    /// Power up + configure the radio for the profile's air params with
    /// the private sync word. Idempotent per profile change.
    bool begin(p2p::AirProfile profile, uint8_t powerDbm = 14);

    /// Channel-activity detection: true when the channel is free to TX.
    bool channelFree();

    /// Blocking TX of one on-air frame (already framed, §10.3).
    bool send(const uint8_t *data, size_t len);

    /// Blocking RX for up to timeoutMs. Returns bytes received (0 =
    /// timeout/CRC/short), filling rssi/snr when requested.
    int receive(uint8_t *buf, size_t maxLen, uint32_t timeoutMs,
                float *rssi = nullptr, float *snr = nullptr);

    /// Radio to sleep + RF switch low + pins parked (v1 prepareSleep
    /// pattern). The next begin() reconfigures.
    void park();

private:
    p2p::AirProfile profile_ = p2p::kProfileLongRange;
    bool up_ = false;
};

} // namespace minilora
