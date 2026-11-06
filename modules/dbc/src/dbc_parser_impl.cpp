// Statement-level recursive descent over the grammar of
// contracts/dbc_subset.md section 1, with the three-tier treatment of
// sections 2 and 3: parse, warn-and-skip, or reject the whole file.
//
// Written by hand rather than taken from a DBC library because every
// permissive parser silently tolerates what it does not model, which is the
// opposite of the fail-closed contract this project needs (research R-11).

#include <cerrno>
#include <cstdlib>
#include <string>

#include "dbc_ast.hpp"

namespace tcu::dbc::detail {
namespace {

/// Every statement keyword the subset knows about, in any tier. Used both to
/// dispatch and to find the next statement during error recovery.
bool isStatementKeyword(const std::string& text) {
  return text == "VERSION" || text == "NS_" || text == "BS_" || text == "BU_" ||
         text == "BO_" || text == "SG_" || text == "VAL_" || text == "CM_" ||
         text == "BA_DEF_" || text == "BA_DEF_DEF_" || text == "BA_" || text == "BA_DEF_REL_" ||
         text == "VAL_TABLE_" || text == "SIG_GROUP_" || text == "EV_" ||
         text == "SIG_VALTYPE_" || text == "SG_MUL_VAL_" || text == "BO_TX_BU_" ||
         text == "ENVVAR_DATA_" || text == "SGTYPE_";
}

/// Tier 2 of research R-02: skipped to the end of the statement, counted, and
/// reported once per kind as a warning. No attribute value is ever interpreted,
/// so GenMsgCycleTime never becomes a timeout (research R-03).
bool isIgnorable(const std::string& text) {
  return text == "CM_" || text == "BA_DEF_" || text == "BA_DEF_DEF_" || text == "BA_" ||
         text == "BA_DEF_REL_" || text == "VAL_TABLE_" || text == "SIG_GROUP_" ||
         text == "EV_" || text == "ENVVAR_DATA_" || text == "SGTYPE_";
}

/// Tier 3: these change what the bits mean, so ignoring one would decode a
/// value wrongly and report it as Valid - the one outcome spec section 1 calls
/// unacceptable.
bool isRejectedFeature(const std::string& text) {
  return text == "SIG_VALTYPE_" || text == "SG_MUL_VAL_" || text == "BO_TX_BU_";
}

class Parser {
 public:
  Parser(const std::vector<Token>& tokens, std::string_view fileLabel,
         const ParseLimits& limits, ParsedFile& out, std::vector<ParseDiagnostic>& diagnostics)
      : tokens_(tokens), fileLabel_(fileLabel), limits_(limits), out_(out),
        diagnostics_(diagnostics) {}

  bool run() {
    while (!atEnd()) {
      const Token& token = peek();
      if (token.kind != TokenKind::Identifier) {
        error(token, DbcErrorCode::DbcSyntax, "expected a statement keyword");
        recover();
        continue;
      }
      parseStatement(token.text);
    }
    return !hasError_;
  }

 private:
  const std::vector<Token>& tokens_;
  std::string_view fileLabel_;
  const ParseLimits& limits_;
  ParsedFile& out_;
  std::vector<ParseDiagnostic>& diagnostics_;
  std::size_t index_{0};
  std::size_t statements_{0};
  bool hasError_{false};
  std::vector<std::string> warnedKinds_;

  const Token& peek(std::size_t ahead = 0) const {
    const std::size_t at = index_ + ahead;
    return at < tokens_.size() ? tokens_[at] : tokens_.back();
  }
  bool atEnd() const { return peek().kind == TokenKind::End; }
  const Token& take() { return tokens_[index_ < tokens_.size() - 1 ? index_++ : index_]; }

  void error(const Token& at, DbcErrorCode code, std::string message) {
    hasError_ = true;
    diagnostics_.push_back(ParseDiagnostic{std::string(fileLabel_), at.line, at.column, code,
                                           Severity::Error, std::move(message)});
  }

  void warnOncePerKind(const Token& at, const std::string& kind) {
    ++out_.ignoredStatements;
    for (const auto& seen : warnedKinds_) {
      if (seen == kind) return;
    }
    warnedKinds_.push_back(kind);
    ++out_.warnings;
    diagnostics_.push_back(ParseDiagnostic{std::string(fileLabel_), at.line, at.column,
                                           DbcErrorCode::None, Severity::Warning,
                                           kind + " statements are not interpreted in v1"});
  }

  /// Advance to the next statement keyword, so one bad statement does not
  /// cascade into a hundred diagnostics (spec section 6.3).
  void recover() {
    if (!atEnd()) ++index_;
    while (!atEnd()) {
      const Token& token = peek();
      if (token.kind == TokenKind::Identifier && isStatementKeyword(token.text) &&
          token.column == 1) {
        return;
      }
      ++index_;
    }
  }

  void skipToSemicolon(const Token& start) {
    while (!atEnd()) {
      if (peek().kind == TokenKind::Semicolon) {
        ++index_;
        return;
      }
      ++index_;
    }
    error(start, DbcErrorCode::DbcSyntax, "statement is not terminated by a semicolon");
  }

  bool expect(TokenKind kind, const char* what) {
    if (peek().kind == kind) {
      ++index_;
      return true;
    }
    error(peek(), DbcErrorCode::DbcSyntax, std::string("expected ") + what);
    return false;
  }

  /// Returns false when the token is not a number at all. A value that
  /// overflows a double comes back as infinity on purpose: the validator turns
  /// a non-finite factor into DBC_RANGE, which is what B-17 asserts.
  bool number(double& value, const char* what) {
    bool negative = false;
    if (peek().kind == TokenKind::Minus) {
      negative = true;
      ++index_;
    } else if (peek().kind == TokenKind::Plus) {
      ++index_;
    }
    if (peek().kind != TokenKind::Number) {
      error(peek(), DbcErrorCode::DbcSyntax, std::string("expected ") + what);
      return false;
    }
    const Token& token = take();
    errno = 0;
    value = std::strtod(token.text.c_str(), nullptr);
    if (negative) value = -value;
    return true;
  }

  void parseStatement(const std::string& keyword);
  void parseVersion();
  void parseNamespace();
  void parseBitTiming();
  void parseNodes();
  void parseMessage();
  void parseSignal();
  void parseValueDescription();
};


void Parser::parseStatement(const std::string& keyword) {
  if (++statements_ > limits_.maxStatements) {
    error(peek(), DbcErrorCode::DbcLimit, "max_statements exceeded");
    recover();
    return;
  }
  if (keyword == "VERSION") return parseVersion();
  if (keyword == "NS_") return parseNamespace();
  if (keyword == "BS_") return parseBitTiming();
  if (keyword == "BU_") return parseNodes();
  if (keyword == "BO_") return parseMessage();
  if (keyword == "SG_") return parseSignal();
  if (keyword == "VAL_") return parseValueDescription();

  const Token& start = peek();
  if (isRejectedFeature(keyword)) {
    error(start, DbcErrorCode::UnsupportedFeature,
          keyword + " is not supported in v1 and changes how bits are interpreted");
    recover();
    return;
  }
  if (isIgnorable(keyword)) {
    warnOncePerKind(start, keyword);
    ++index_;
    skipToSemicolon(start);
    return;
  }
  error(start, DbcErrorCode::DbcSyntax,
        "unknown statement '" + keyword.substr(0, 64) + "' may affect decoding");
  recover();
}

void Parser::parseVersion() {
  ++index_;
  if (peek().kind != TokenKind::String) {
    error(peek(), DbcErrorCode::DbcSyntax, "expected a quoted version string after VERSION");
    recover();
    return;
  }
  ++index_;
}

// NS_ content is a bare identifier list and those identifiers are themselves
// keyword-shaped (CM_, BA_DEF_, VAL_). The subset therefore skips to the first
// statement that may legally follow it, which is BS_, BU_ or BO_.
void Parser::parseNamespace() {
  ++index_;
  if (!expect(TokenKind::Colon, "':' after NS_")) {
    recover();
    return;
  }
  while (!atEnd()) {
    const Token& token = peek();
    if (token.kind != TokenKind::Identifier) break;
    if (token.text == "BS_" || token.text == "BU_" || token.text == "BO_") break;
    ++index_;
  }
}

void Parser::parseBitTiming() {
  ++index_;
  if (!expect(TokenKind::Colon, "':' after BS_")) {
    recover();
    return;
  }
  while (!atEnd()) {
    const TokenKind kind = peek().kind;
    if (kind == TokenKind::Number || kind == TokenKind::Colon || kind == TokenKind::Comma) {
      ++index_;
      continue;
    }
    break;
  }
}

void Parser::parseNodes() {
  ++index_;
  if (!expect(TokenKind::Colon, "':' after BU_")) {
    recover();
    return;
  }
  while (!atEnd()) {
    const Token& token = peek();
    if (token.kind != TokenKind::Identifier || isStatementKeyword(token.text)) break;
    ++index_;
  }
}

void Parser::parseMessage() {
  const Token& start = peek();
  ++index_;

  RawMessage message;
  message.line = start.line;
  message.column = start.column;

  if (!number(message.rawId, "a decimal CAN identifier after BO_")) {
    recover();
    return;
  }
  if (peek().kind != TokenKind::Identifier) {
    error(peek(), DbcErrorCode::DbcSyntax, "expected a message name");
    recover();
    return;
  }
  message.name = take().text;
  if (!expect(TokenKind::Colon, "':' after the message name")) {
    recover();
    return;
  }
  if (!number(message.length, "a payload length")) {
    recover();
    return;
  }
  // The transmitter node is accepted and not interpreted; v1 decodes, it does
  // not model who sends what. Bounded to the BO_ line: a BO_ without a
  // transmitter must not swallow the next statement.
  if (peek().kind == TokenKind::Identifier && peek().line == start.line &&
      !isStatementKeyword(peek().text)) {
    ++index_;
  }

  if (out_.messages.size() >= limits_.maxMessages) {
    error(start, DbcErrorCode::DbcLimit, "max_messages exceeded");
    recover();
    return;
  }
  out_.messages.push_back(std::move(message));
}

void Parser::parseSignal() {
  const Token& start = peek();
  ++index_;

  if (out_.messages.empty()) {
    error(start, DbcErrorCode::DbcSyntax, "SG_ appears before any BO_ statement");
    recover();
    return;
  }

  RawSignal signal;
  signal.line = start.line;
  signal.column = start.column;

  if (peek().kind != TokenKind::Identifier) {
    error(peek(), DbcErrorCode::DbcSyntax, "expected a signal name");
    recover();
    return;
  }
  signal.name = take().text;

  // Optional multiplex indicator: M for the selector, mN for a branch.
  if (peek().kind == TokenKind::Identifier) {
    const std::string& indicator = peek().text;
    if (indicator == "M") {
      signal.muxRole = MuxRole::Selector;
      ++index_;
    } else if (indicator.size() > 1 && indicator[0] == 'm') {
      bool digitsOnly = true;
      for (std::size_t i = 1; i < indicator.size(); ++i) {
        if (indicator[i] < '0' || indicator[i] > '9') digitsOnly = false;
      }
      if (digitsOnly) {
        signal.muxRole = MuxRole::Branch;
        signal.muxValue = static_cast<std::uint32_t>(std::strtoul(indicator.c_str() + 1, nullptr, 10));
        ++index_;
      }
    }
  }

  if (!expect(TokenKind::Colon, "':' after the signal name")) {
    recover();
    return;
  }
  if (!number(signal.startBit, "a start bit")) {
    recover();
    return;
  }
  if (!expect(TokenKind::Pipe, "'|' between start bit and width")) {
    recover();
    return;
  }
  if (!number(signal.width, "a bit width")) {
    recover();
    return;
  }
  if (!expect(TokenKind::At, "'@' before the byte order")) {
    recover();
    return;
  }
  double order = 0;
  if (!number(order, "a byte order, 0 for Motorola or 1 for Intel")) {
    recover();
    return;
  }
  if (order == 1) {
    signal.byteOrder = ByteOrder::Intel;
  } else if (order == 0) {
    signal.byteOrder = ByteOrder::Motorola;
  } else {
    error(start, DbcErrorCode::DbcSignal, "byte order must be @0 (Motorola) or @1 (Intel)");
    recover();
    return;
  }

  if (peek().kind == TokenKind::Plus) {
    signal.isSigned = false;
    ++index_;
  } else if (peek().kind == TokenKind::Minus) {
    signal.isSigned = true;
    ++index_;
  } else {
    error(peek(), DbcErrorCode::DbcSyntax, "expected '+' (unsigned) or '-' (signed)");
    recover();
    return;
  }

  if (!expect(TokenKind::LParen, "'(' before factor and offset")) {
    recover();
    return;
  }
  if (!number(signal.factor, "a factor")) {
    recover();
    return;
  }
  if (!expect(TokenKind::Comma, "',' between factor and offset")) {
    recover();
    return;
  }
  if (!number(signal.offset, "an offset")) {
    recover();
    return;
  }
  if (!expect(TokenKind::RParen, "')' after factor and offset")) {
    recover();
    return;
  }
  if (!expect(TokenKind::LBracket, "'[' before the range")) {
    recover();
    return;
  }
  if (!number(signal.minimum, "a minimum")) {
    recover();
    return;
  }
  if (!expect(TokenKind::Pipe, "'|' between minimum and maximum")) {
    recover();
    return;
  }
  if (!number(signal.maximum, "a maximum")) {
    recover();
    return;
  }
  if (!expect(TokenKind::RBracket, "']' after the range")) {
    recover();
    return;
  }
  if (peek().kind != TokenKind::String) {
    error(peek(), DbcErrorCode::DbcSyntax, "expected a quoted unit");
    recover();
    return;
  }
  signal.unit = take().text;

  // Receiver list: accepted, not interpreted. Bounded to this signal's line -
  // DBC writes SG_ on one line, and an unbounded list would silently eat the
  // next statement, including one that should have been rejected.
  while (!atEnd()) {
    const Token& token = peek();
    if (token.line != start.line) break;
    if (token.kind == TokenKind::Comma) {
      ++index_;
      continue;
    }
    if (token.kind == TokenKind::Identifier && !isStatementKeyword(token.text)) {
      ++index_;
      continue;
    }
    break;
  }

  RawMessage& owner = out_.messages.back();
  if (owner.signals.size() >= limits_.maxSignalsPerMessage) {
    error(start, DbcErrorCode::DbcLimit, "max_signals_per_message exceeded");
    recover();
    return;
  }
  owner.signals.push_back(std::move(signal));
}

void Parser::parseValueDescription() {
  const Token& start = peek();
  ++index_;

  RawValueTable table;
  table.line = start.line;
  table.column = start.column;

  if (!number(table.rawId, "a message identifier after VAL_")) {
    recover();
    return;
  }
  if (peek().kind != TokenKind::Identifier) {
    error(peek(), DbcErrorCode::DbcSyntax, "expected a signal name after the VAL_ identifier");
    recover();
    return;
  }
  table.signalName = take().text;

  // A value TABLE reference instead of inline pairs: v1 resolves no table
  // names, so this is an enum error rather than a silently empty enum (R-02).
  if (peek().kind == TokenKind::Identifier) {
    error(peek(), DbcErrorCode::DbcEnum,
          "VAL_ references a value table by name, which v1 does not resolve");
    recover();
    return;
  }

  while (!atEnd() && peek().kind != TokenKind::Semicolon) {
    double value = 0;
    if (!number(value, "an enum value")) {
      recover();
      return;
    }
    if (peek().kind != TokenKind::String) {
      error(peek(), DbcErrorCode::DbcSyntax, "expected a quoted label after the enum value");
      recover();
      return;
    }
    table.pairs.emplace_back(static_cast<std::int64_t>(value), take().text);
  }
  if (!expect(TokenKind::Semicolon, "';' at the end of the VAL_ statement")) {
    recover();
    return;
  }
  out_.valueTables.push_back(std::move(table));
}

}  // namespace

bool parseTokens(const std::vector<Token>& tokens, std::string_view fileLabel,
                 const ParseLimits& limits, ParsedFile& out,
                 std::vector<ParseDiagnostic>& diagnostics) {
  Parser parser(tokens, fileLabel, limits, out, diagnostics);
  return parser.run();
}

}  // namespace tcu::dbc::detail
