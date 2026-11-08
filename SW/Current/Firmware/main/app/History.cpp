#include "History.hpp"

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <cstring>

namespace minilora {

namespace hist {
namespace {

constexpr const char *kNs = "hist";
constexpr const char *kKey = "hist";

struct Blob {
    uint8_t count;                   // valid entries (<= kSlots)
    uint8_t writeIdx;                // next slot to overwrite
    uint8_t entries[kSlots][kEntrySize];
};
static_assert(sizeof(Blob) == 2 + kSlots * kEntrySize, "hist blob layout");

bool load(Blob &b)
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(Blob);
    const bool ok = nvs_get_blob(h, kKey, &b, &len) == ESP_OK &&
                    len == sizeof(Blob) && b.count <= kSlots;
    nvs_close(h);
    if (!ok) {
        b = {};
    }
    return ok;
}

void store(const Blob &b)
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, kKey, &b, sizeof(Blob));
    nvs_commit(h);
    nvs_close(h);
}

void putEntry(uint8_t *e, int32_t latMicro, int32_t lonMicro,
              uint32_t epochSec, uint8_t sats)
{
    std::memcpy(e, &latMicro, 4);
    std::memcpy(e + 4, &lonMicro, 4);
    std::memcpy(e + 8, &epochSec, 4);
    e[12] = sats;
}

} // namespace

void append(int32_t latMicro, int32_t lonMicro, uint32_t epochSec,
            uint8_t sats)
{
    Blob b = {};
    load(b);
    putEntry(b.entries[b.writeIdx % kSlots], latMicro, lonMicro, epochSec,
             sats);
    b.writeIdx = (b.writeIdx + 1) % kSlots;
    if (b.count < kSlots) {
        b.count++;
    }
    store(b);
}

size_t count()
{
    Blob b = {};
    return load(b) ? b.count : 0;
}

size_t buildFrame(uint8_t *out, size_t maxLen)
{
    Blob b = {};
    if (!load(b) || b.count == 0 || maxLen < 2 + 2 * kEntrySize) {
        return 0;
    }
    // The two most recent entries, OLDEST first: slot order is a ring, so
    // the newest is (writeIdx - 1) mod slots.
    const size_t newest = (b.writeIdx + kSlots - 1) % kSlots;
    const size_t older = (b.writeIdx + kSlots - 2) % kSlots;
    size_t n = 0;
    out[n++] = kSchema;
    if (b.count >= 2) {
        out[n++] = 2;
        std::memcpy(out + n, b.entries[older], kEntrySize);
        n += kEntrySize;
        std::memcpy(out + n, b.entries[newest], kEntrySize);
        n += kEntrySize;
    } else {
        out[n++] = 1;
        std::memcpy(out + n, b.entries[newest], kEntrySize);
        n += kEntrySize;
    }
    return n;
}

} // namespace hist

} // namespace minilora
