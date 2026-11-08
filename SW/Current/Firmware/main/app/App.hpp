// The v2 state machine: wake → (config?) → sense → transmit → sleep.
// Mirrors the v1 flow (docs/FLOW.md in the Arduino project) with explicit
// failure paths and watchdog feeding at every phase boundary.

#pragma once

#include <cstdint>

namespace minilora {

/// Runs one full cycle; always ends in deep sleep (never returns).
/// `recovery` forces the minimal cycle (boot-loop guard).
void runCycle(bool recovery);

} // namespace minilora
