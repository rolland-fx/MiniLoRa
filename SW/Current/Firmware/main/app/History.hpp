// Store-and-forward position history (Milestone 3).
//
// Every successful GPS fix is appended to a small NVS ring buffer; on
// GPS-less cycles the tracker emits a 28-byte fPort 4 frame carrying the
// TWO most recent fixes (position + GPS time), so a tracker that was
// underground for a day still reports where it was, and when:
//
//   byte 0     schema  = 0x01
//   byte 1     count   = 1..2 (entries that follow, oldest first)
//   entry (13 bytes, ×2):
//     0-3   latitude   int32 LE microdegrees
//     4-7   longitude  int32 LE microdegrees
//     8-11  time       uint32 LE unix epoch of the fix
//     12    sats       uint8
//
// NVS layout: one blob ("hist") of kHistSlots × 13 bytes + a 1-byte write
// index; rewritten per fix (worst case ~96×28 B/day — negligible wear).
// Emission is throttled (every kSendEvery no-fix cycles) to bound the
// energy of a long underground stay.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../domain/Payload.hpp"

namespace minilora {

namespace hist {

inline constexpr size_t kEntrySize = 13;
inline constexpr size_t kSlots = 8;
inline constexpr uint8_t kSchema = 0x01;
/// Emit a history frame every kSendEvery GPS-less cycles (when history
/// exists). 1 = every cycle.
inline constexpr uint8_t kSendEvery = 4;

/// Append a fix to the ring buffer (call only with gpsValid).
void append(int32_t latMicro, int32_t lonMicro, uint32_t epochSec,
            uint8_t sats);

/// Build the fPort-4 frame from the two most recent entries.
/// Returns the frame length (0 = no history worth sending).
size_t buildFrame(uint8_t *out, size_t maxLen);

/// Number of entries currently buffered.
size_t count();

} // namespace hist

} // namespace minilora
