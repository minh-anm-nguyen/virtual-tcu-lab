#pragma once

// An injectable wall clock, so a formatter assertion can be a byte-exact
// golden line instead of a timestamp pattern (research R-17).

#include <chrono>

namespace tcu::logging::test {

class FakeClock {
 public:
  // 2026-10-30T01:00:00.000Z - the instant the spec's section 16 examples use.
  static std::chrono::system_clock::time_point epoch() {
    // 1793322000 = 2026-10-30T01:00:00Z. Computed once here rather than parsed,
    // so the fixture needs no date library.
    return std::chrono::system_clock::time_point(std::chrono::seconds(1793322000));
  }

  std::chrono::system_clock::time_point operator()() const { return now_; }

  void set(std::chrono::system_clock::time_point tp) { now_ = tp; }
  void advance(std::chrono::milliseconds d) { now_ += d; }

 private:
  std::chrono::system_clock::time_point now_{epoch()};
};

}  // namespace tcu::logging::test
