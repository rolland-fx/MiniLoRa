// P2P star gateway — ICD §10. One boot == one wake window.
//
// The gateway is a tracker with a radio frontend: it sends its own 28-byte
// frame exactly like a leaf, and additionally listens on the slot grid and
// forwards every heard frame to the LAN server over the existing
// /data?device=<origin> push path (the server already keys readings by the
// device parameter; the §1 body carries the origin's gps_time so frame_ts
// lands correctly).
//
// Bootstrapping (bench step 1 → step 2): a gateway that has learned
// nothing stays awake for GW_LEARN_MS listening continuously (forwarding
// as it hears). Once a leaf has been silent past the learn window, the
// gateway switches to the slot grid: deep sleep between catch slots
// (every 60 s), predicted leaf windows from the learned (origin, phase,
// sleep_min) table, and its own hash slot. All schedule state lives in
// RTC RAM and survives deep sleep.

#pragma once

#include <cstdint>

#include "../domain/Config.hpp"

namespace minilora {
namespace p2pgw {

struct Context {
    const DeviceConfig &cfg;
    // The gateway's own telemetry, already encoded (§1 28-byte body).
    const uint8_t *ownPayload;
    size_t ownLen;
    // True when the battery forbids WiFi pushes this wake (RX-only).
    bool wifiForbidden;
};

/// Run this wake's gateway phase: RX windows, forwarding, own TX,
/// forward-queue flush, and the deep-sleep arming for the next window.
/// Never returns normally — it arms sleep and returns when runCycle
/// should skip its own sleep phase (prepareSleep already called).
void wakePhase(const Context &ctx, bool *slept);

} // namespace p2pgw
} // namespace minilora
