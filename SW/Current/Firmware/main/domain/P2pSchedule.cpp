#include "P2pSchedule.hpp"

#include <limits>

namespace minilora {
namespace p2p {

namespace {

// FNV-1a 32-bit over 8 bytes (origin id + superframe counter).
uint32_t fnv1a(uint32_t a, uint32_t b)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint8_t>(a >> (8 * i));
        h *= 16777619u;
    }
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint8_t>(b >> (8 * i));
        h *= 16777619u;
    }
    return h;
}

} // namespace

const AirParams &airParams(AirProfile p)
{
    // nSlots = 60 s / slotLenMs; split 80/10/10, min 2 catch (ICD §10.4).
    static const AirParams kTable[3] = {
        // freq      sf  bw    slotMs  data  catch
        {869.525f, 12, 125, 3000, 16, 2},  // long-range: 20 slots
        {869.525f, 10, 125, 800, 60, 8},   // balanced:   75 slots
        {869.525f, 7, 125, 130, 369, 46},  // bench:     461 slots
    };
    return kTable[p < kProfileBench ? p : kProfileBench];
}

uint16_t hashSlot(uint32_t originId, uint32_t superframe, uint16_t nData)
{
    if (nData == 0) {
        return 0;
    }
    return static_cast<uint16_t>(fnv1a(originId, superframe) % nData);
}

uint32_t superframeOf(uint32_t epochSec)
{
    return epochSec / kSuperframeSec;
}

uint32_t superframeStart(uint32_t epochSec)
{
    return epochSec - (epochSec % kSuperframeSec);
}

bool isCatchSlot(uint32_t epochSec, AirProfile p)
{
    const AirParams &ap = airParams(p);
    const uint64_t inSuperframeMs =
        static_cast<uint64_t>(epochSec % kSuperframeSec) * 1000;
    const uint16_t idx = static_cast<uint16_t>(inSuperframeMs / ap.slotLenMs);
    return idx >= ap.nData && idx < ap.nData + ap.nCatch;
}

uint64_t nextDataSlotMs(uint32_t originId, AirProfile p, uint64_t notBeforeMs)
{
    const AirParams &ap = airParams(p);
    // Walk superframes forward until the hash-assigned data slot starts
    // at/after notBeforeMs. One iteration covers nearly every case; the
    // loop only spins when the hash slot falls inside the slot we are
    // still in.
    uint64_t sfStartMs = static_cast<uint64_t>(notBeforeMs / (kSuperframeSec * 1000)) *
                         kSuperframeSec * 1000;
    for (int guard = 0; guard < 4; ++guard) {
        const uint32_t sf = static_cast<uint32_t>(sfStartMs / (kSuperframeSec * 1000));
        const uint16_t slot = hashSlot(originId, sf, ap.nData);
        const uint64_t startMs = sfStartMs + static_cast<uint64_t>(slot) * ap.slotLenMs;
        if (startMs >= notBeforeMs) {
            return startMs;
        }
        sfStartMs += kSuperframeSec * 1000;
    }
    return notBeforeMs; // unreachable in practice; caller TXes immediately
}

uint64_t nextCatchSlotMs(AirProfile p, uint64_t notBeforeMs)
{
    const AirParams &ap = airParams(p);
    const uint64_t sfMs = kSuperframeSec * 1000;
    uint64_t sfStartMs = static_cast<uint64_t>(notBeforeMs / sfMs) * sfMs;
    for (int guard = 0; guard < 4; ++guard) {
        for (uint16_t c = 0; c < ap.nCatch; ++c) {
            const uint64_t startMs = sfStartMs +
                static_cast<uint64_t>(ap.nData + c) * ap.slotLenMs;
            const uint64_t endMs = startMs + ap.slotLenMs;
            if (endMs > notBeforeMs && startMs + 500 >= notBeforeMs) {
                // the leaf must still fit its sensor read before TX:
                // require the slot to start at least ~0.5 s out
                return startMs;
            }
        }
        sfStartMs += sfMs;
    }
    return notBeforeMs + sfMs;
}

} // namespace p2p
} // namespace minilora
