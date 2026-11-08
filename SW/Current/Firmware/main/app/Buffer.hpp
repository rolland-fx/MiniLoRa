// Store-and-forward telemetry buffer (offline points).
//
// When no transport manages to send a cycle's payload (LoRaWAN not
// joined/failed AND WiFi push failed or unconfigured), the 28-byte fPort-1
// frame is appended to an NVS ring (kMaxFrames entries, drop-oldest). On
// the first later cycle where a transport works, the buffer drains:
//
//   - WiFi: everything in ONE bulk POST to /data (the server accepts
//     n x 28 B concatenated frames) — seconds, no airtime.
//   - LoRaWAN: the two oldest frames per wake, sent as normal fPort-1
//     telemetry (duty-cycle-safe; 50 points ≈ 25 wakes).
//
// Frames are replayed unmodified: no ICD change, every consumer (server,
// TTN decoder, app, HA bridge) already understands them, and each carries
// its original GPS epoch. The server's seen_at is the flush time — the
// original moment lives in the row's gps_time.
//
// NVS layout: one blob ("buf") of 2 + kMaxFrames × 28 bytes (write index,
// count, frames), rewritten only when the buffer changes — never on
// healthy cycles.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../domain/Payload.hpp"

namespace minilora {

namespace buf {

inline constexpr size_t kMaxFrames = 50;

/// Buffered points waiting for a transport.
size_t count();

/// Append one kPayloadSize frame; drops the oldest when full.
void append(const uint8_t *frame);

/// Copy up to maxFrames oldest-first into out (needs maxFrames*kPayloadSize
/// bytes). Returns the number of BYTES written (0 = empty).
size_t readOldest(uint8_t *out, size_t maxFrames);

/// Remove the n oldest frames (call only after they were sent).
void dropOldest(size_t n);

} // namespace buf

} // namespace minilora
