// P2P star air schedule — GPS-anchored slot grid (ICD §10.4).
//
// All nodes derive the grid from absolute time: superframe =
// floor(gps_epoch / 60 s), slots are consecutive regions inside it.
// No beacons, no sync protocol; a GPS fix re-tightens every RTC to the
// same second and the guard band absorbs the rest.
//
// Pure math — no ESP, no radio — unit-testable on the host.

#pragma once

#include <cstdint>

namespace minilora {
namespace p2p {

// Air profiles (ICD §10.2) — cfgP2PProfile values.
enum AirProfile : uint8_t {
    kProfileLongRange = 0, // SF12 @ 869.525 — default
    kProfileBalanced = 1,  // SF10 @ 869.525
    kProfileBench = 2,     // SF7  @ 869.525
};

struct AirParams {
    float    freqMHz;
    uint8_t  sf;
    uint16_t bwKhz;
    uint32_t slotLenMs;  // slot = airtime × 1.5, rounded to a clean grid
    uint16_t nData;      // data slots per superframe (hash-assigned)
    uint16_t nCatch;     // catch slots per superframe (always-on at the GW)
    // reserve = the rest of the 60 s superframe
};

/// ICD §10.2 table. nData/nCatch follow the 80/10/10 split of the 60 s
/// superframe, minimum 2 catch slots.
const AirParams &airParams(AirProfile p);

constexpr uint32_t kSuperframeSec = 60;

/// FNV-1a over (origin, superframe) — the slot-assignment hash. It
/// changes every superframe, so a collision is transient by design.
uint16_t hashSlot(uint32_t originId, uint32_t superframe, uint16_t nData);

uint32_t superframeOf(uint32_t epochSec);    // floor(epoch / 60)
uint32_t superframeStart(uint32_t epochSec); // floor to the 60 s boundary

/// Slot layout inside a superframe: [0, nData) data, then nCatch catch,
/// then reserve. Slot i starts at superframeStart + i * slotLenMs.
bool isCatchSlot(uint32_t epochSec, AirProfile p);

/// Epoch-ms of the start of the NEXT data slot assigned to `originId`
/// whose whole slot lies at/after `notBeforeMs`. Deterministic on both
/// the leaf and the gateway.
uint64_t nextDataSlotMs(uint32_t originId, AirProfile p,
                        uint64_t notBeforeMs);

/// Epoch-ms of the start of the NEXT catch slot at/after `notBeforeMs`.
uint64_t nextCatchSlotMs(AirProfile p, uint64_t notBeforeMs);

} // namespace p2p
} // namespace minilora
