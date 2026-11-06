// Fuzz target: the DBC parser over arbitrary bytes (NFR-05, research R-21).
//
// The only invariant asserted is the one that matters: loadDbcBytes must come
// back with EITHER a usable database OR at least one error diagnostic, for any
// input at all, and must never throw. Anything else - a crash, a hang, an
// out-of-bounds read, an unsigned overflow - is caught by the sanitizers this
// target is always built with.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "tcu/dbc/dbc_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Keep the limits small so a fuzz case cannot spend its whole budget inside
  // one pathological file; the limit paths are covered by unit tests.
  tcu::dbc::ParseLimits limits;
  limits.dbcMaxBytes = 64 * 1024;
  limits.maxMessages = 64;
  limits.maxSignalsPerMessage = 32;
  limits.maxTotalSignals = 512;
  limits.maxStatements = 2048;

  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const tcu::dbc::LoadResult result = tcu::dbc::loadDbcBytes(text, "fuzz.dbc", limits);

  if (result.db != nullptr) {
    // A published database must have no error diagnostic and must be queryable
    // without reading out of bounds (invariant D-03).
    for (const auto& diagnostic : result.diagnostics) {
      if (diagnostic.severity == tcu::dbc::Severity::Error) __builtin_trap();
    }
    if (result.db->hash().size() != 64) __builtin_trap();
    for (const auto& entry : result.db->messages()) {
      const auto& message = entry.second;
      if (message.expectedLength > 8) __builtin_trap();
      for (const auto& meta : message.signals) {
        if (meta.name.empty()) __builtin_trap();
      }
    }
  } else if (result.diagnostics.empty()) {
    // No database and no reason why: that would be a silent rejection, which
    // FR-15 and NFR-10 forbid.
    __builtin_trap();
  }
  return 0;
}
