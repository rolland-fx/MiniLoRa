// Sleep policy: the single owner of power transitions (Roadmap §3).
// v1 prepareSleep() semantics: timer + EXT1 wake, GPIO holds, RTC isolate.

#pragma once

#include <cstdint>

namespace minilora {

void prepareSleep(uint32_t sleepMs);

/// Asset-mode variant (P5): keeps GPIO4 (LIS3DH INT1, active-low
/// open-drain) alive as a second EXT1 wake source alongside the button
/// (GPIO7). Disambiguate with esp_sleep_get_ext1_wakeup_status().
void prepareSleepMotion(uint32_t sleepMs);

/// Feed the task watchdog (called at state-machine phase boundaries and
/// inside bounded loops — never free-running).
void feedWatchdog();

} // namespace minilora
