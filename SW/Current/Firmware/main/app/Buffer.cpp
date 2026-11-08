#include "Buffer.hpp"

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <cstring>

namespace minilora {

namespace buf {
namespace {

constexpr const char *kNs = "buf";
constexpr const char *kKey = "buf";
constexpr size_t kFrameSize = kPayloadSize;  // 28

struct Blob {
    uint8_t writeIdx;  // next slot to overwrite, always < kMaxFrames
    uint8_t count;     // valid frames (<= kMaxFrames)
    uint8_t frames[kMaxFrames][kFrameSize];
};
static_assert(sizeof(Blob) == 2 + kMaxFrames * kFrameSize, "buf blob layout");

bool load(Blob &b)
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(Blob);
    const bool ok = nvs_get_blob(h, kKey, &b, &len) == ESP_OK &&
                    len == sizeof(Blob) && b.count <= kMaxFrames &&
                    b.writeIdx < kMaxFrames;
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

size_t oldestIdx(const Blob &b)
{
    return (b.writeIdx + kMaxFrames - b.count) % kMaxFrames;
}

} // namespace

size_t count()
{
    Blob b = {};
    return load(b) ? b.count : 0;
}

void append(const uint8_t *frame)
{
    Blob b = {};
    load(b);
    std::memcpy(b.frames[b.writeIdx], frame, kFrameSize);
    b.writeIdx = (b.writeIdx + 1) % kMaxFrames;
    if (b.count < kMaxFrames) {
        b.count++;  // full ring: the oldest slot was just overwritten
    }
    store(b);
}

size_t readOldest(uint8_t *out, size_t maxFrames)
{
    Blob b = {};
    if (!load(b)) {
        return 0;
    }
    const size_t n = b.count < maxFrames ? b.count : maxFrames;
    size_t idx = oldestIdx(b);
    size_t bytes = 0;
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(out + bytes, b.frames[idx], kFrameSize);
        bytes += kFrameSize;
        idx = (idx + 1) % kMaxFrames;
    }
    return bytes;
}

void dropOldest(size_t n)
{
    Blob b = {};
    if (!load(b) || n == 0) {
        return;
    }
    if (n > b.count) {
        n = b.count;
    }
    b.count -= n;  // the start index is derived from writeIdx - count
    store(b);
}

} // namespace buf

} // namespace minilora
