#pragma once

#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace tcu::demo {

// Runs the spec section 20 scenario end to end and returns the final metrics.
// Shared by main.cpp and the smoke test, so the demo cannot rot silently.
// Logs one structured line per publish and per callback when `verbose`.
tcu::eventbus::MetricsSnapshot run_demo(tcu::eventbus::StopMode mode, bool verbose);

}  // namespace tcu::demo
