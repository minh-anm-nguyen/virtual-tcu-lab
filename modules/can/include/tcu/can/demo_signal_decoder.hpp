#pragma once

// Raw payload bytes to typed physical values, for the seven synthetic
// identifiers of frame_catalog.hpp.
//
// Deliberately specific to that table. A general DBC parser is project 06
// (ADR-02), and the interface is virtual so project 06 can replace the
// implementation behind the same transport, cache and service.

#include <vector>

#include "tcu/can/can_types.hpp"

namespace tcu::can {

struct DecodeResult {
  bool accepted{false};
  /// A closed enum, not the spec snippet's std::string: a reason built from
  /// rejected input is a log-injection path, and it also has to serve as a
  /// metric label (declared in plan.md Complexity Tracking).
  RejectReason reason{RejectReason::None};
  /// Empty whenever accepted is false - atomic frame reject (research R-14).
  std::vector<SignalSample> updates;
};

class IDemoSignalDecoder {
 public:
  virtual ~IDemoSignalDecoder() = default;
  virtual DecodeResult decode(const CanMessage& frame) = 0;
};

class DemoSignalDecoder final : public IDemoSignalDecoder {
 public:
  /// Pure apart from reading the catalog: no clock, no cache, no allocation
  /// beyond the returned vector. `updates[i].last_good_at` is copied from
  /// `frame.received_at`, so the decoder never decides what time it is.
  DecodeResult decode(const CanMessage& frame) override;
};

}  // namespace tcu::can
