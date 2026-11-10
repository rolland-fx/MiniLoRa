// GPS-anchored wall clock (shared by the P2P modes).
//
// The system clock is set from GPS (or DeviceTimeAns) via settimeofday and
// kept across deep sleep by the RTC timer (App.cpp owns the validity flag
// and the anchoring). Millisecond precision — the trek window grid and the
// leaf slot math both key on this clock, NOT on esp_timer (which resets on
// every deep sleep and would desync the two devices' grids).

#pragma once

#include <cstdint>
#include <sys/time.h>

namespace minilora {

inline uint64_t rtcSystemMs()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
           static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

} // namespace minilora
