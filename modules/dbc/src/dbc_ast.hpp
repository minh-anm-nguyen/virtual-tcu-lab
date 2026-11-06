#pragma once

// The parse tree. Mirrors data-model.md section 1.2 exactly: nothing here is
// validated yet, so every field can hold whatever the file said, and the
// validator is the only place that decides what is legal.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dbc_lexer.hpp"
#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc::detail {

struct RawSignal {
  std::string name;
  /// As written. Validated against the message length later (invariant D-07).
  double startBit{0};
  double width{0};
  ByteOrder byteOrder{ByteOrder::Intel};
  bool isSigned{false};
  double factor{1.0};
  double offset{0.0};
  double minimum{0.0};
  double maximum{0.0};
  std::string unit;
  MuxRole muxRole{MuxRole::None};
  std::uint32_t muxValue{0};
  std::vector<std::pair<std::int64_t, std::string>> valueDescriptions;
  std::uint32_t line{0};
  std::uint32_t column{0};
};

struct RawMessage {
  /// Exactly as written in BO_, flag bit included.
  double rawId{0};
  std::uint32_t arbitrationId{0};
  bool isExtended{false};
  std::string name;
  double length{0};
  std::vector<RawSignal> signals;
  std::uint32_t line{0};
  std::uint32_t column{0};
};

/// A VAL_ statement, kept aside until validation so an enum naming an unknown
/// signal is reported at the VAL_ line rather than silently dropped (D-11).
struct RawValueTable {
  double rawId{0};
  std::string signalName;
  std::vector<std::pair<std::int64_t, std::string>> pairs;
  std::uint32_t line{0};
  std::uint32_t column{0};
};

struct ParsedFile {
  std::vector<RawMessage> messages;
  std::vector<RawValueTable> valueTables;
  std::size_t ignoredStatements{0};
  std::size_t warnings{0};
};

/// Parses the token stream. Collects every problem it can rather than stopping
/// at the first (spec section 6.3); returns false when at least one
/// Severity::Error diagnostic was produced.
bool parseTokens(const std::vector<Token>& tokens, std::string_view fileLabel,
                 const ParseLimits& limits, ParsedFile& out,
                 std::vector<ParseDiagnostic>& diagnostics);

}  // namespace tcu::dbc::detail
