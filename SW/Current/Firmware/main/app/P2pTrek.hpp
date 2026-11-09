// P2P trek mode — two-device bilateral beacon (ICD §10.8).
//
// Both devices wake on the same GPS-anchored window (period = the sleep
// table), exchange their §1 frames in id-ordered sub-slots, and sleep.
// The peer's frame is relayed to the phone over BLE (§10.9) so both maps
// show both tracks. Contact state, adaptive power and the role live in
// RTC RAM; the mesh PSK is the pairing.

#pragma once

#include "../domain/Config.hpp"

namespace minilora {
namespace p2ptrek {

struct Context {
    const DeviceConfig &cfg;
    const uint8_t *ownPayload; // the §1 body built this cycle
    size_t ownLen;
    bool batCritical;
};

/// Run this wake's trek phase: wait for the sub-slots, TX own frame,
/// RX the peer's, adapt power, relay the peer frame over BLE, arm sleep
/// for the next window. Never returns to the leaf flow.
void wakePhase(const Context &ctx, bool *slept);

/// Next trek window start (epoch ms) at/after notBeforeMs, on the
/// period grid (the sleep table). Used by the sleep alignment.
uint64_t nextWindowStartMs(uint32_t periodMs, uint64_t notBeforeMs);

} // namespace p2ptrek
} // namespace minilora
