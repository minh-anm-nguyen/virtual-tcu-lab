#pragma once

// stdout, one JSONL line per record - the same bytes the file sink writes
// (research R-18). A second human-readable formatter would be a second
// escaping implementation to keep correct, for a convenience nobody asked for;
// readability in the demo comes from jq.
//
// The console on/off switch is NOT here: it lives in AsyncLogger, which is the
// only caller, so there is one source of truth for a hot-reloadable setting
// rather than two that can disagree.

#include <string_view>

#include "tcu/logging/log_sink.hpp"

namespace tcu::logging {

class ConsoleSink final : public ILogSink {
 public:
  std::string_view name() const noexcept override {
    return sinkKindName(SinkKind::Console);
  }
  SinkKind kind() const noexcept override { return SinkKind::Console; }

  bool write(std::string_view line) override;
  void flush() override;
  SinkHealth health() const noexcept override { return SinkHealth::Healthy; }
};

}  // namespace tcu::logging
