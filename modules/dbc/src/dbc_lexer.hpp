#pragma once

// Tokens with 1-based line and column, so every diagnostic can be navigated to
// (invariant D-01). Single pass, no recursion, no allocation per token beyond
// the token's own text.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc::detail {

enum class TokenKind {
  End,
  Identifier,
  Number,
  String,
  Colon,
  Pipe,
  At,
  Plus,
  Minus,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
};

struct Token {
  TokenKind kind{TokenKind::End};
  std::string text;
  std::uint32_t line{1};
  std::uint32_t column{1};
};

/// Tokenizes the whole input up front. Returns false when a hard lexical rule
/// was broken (NUL byte, unterminated string, token over the limit, unknown
/// character); `diagnostics` then holds the reason and tokens are unusable.
bool tokenize(std::string_view text, std::string_view fileLabel, const ParseLimits& limits,
              std::vector<Token>& tokens, std::vector<ParseDiagnostic>& diagnostics);

}  // namespace tcu::dbc::detail
