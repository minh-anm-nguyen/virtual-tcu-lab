#include "dbc_lexer.hpp"

namespace tcu::dbc::detail {
namespace {

bool isIdentStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isIdentChar(char c) {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

void addError(std::vector<ParseDiagnostic>& diagnostics, std::string_view fileLabel,
              std::uint32_t line, std::uint32_t column, DbcErrorCode code,
              std::string message) {
  diagnostics.push_back(ParseDiagnostic{std::string(fileLabel), line, column, code,
                                        Severity::Error, std::move(message)});
}

}  // namespace

bool tokenize(std::string_view text, std::string_view fileLabel, const ParseLimits& limits,
              std::vector<Token>& tokens, std::vector<ParseDiagnostic>& diagnostics) {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::size_t i = 0;

  const auto advance = [&](std::size_t count) {
    for (std::size_t n = 0; n < count; ++n) {
      if (text[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
      ++i;
    }
  };

  const auto push = [&](TokenKind kind, std::string value, std::uint32_t tokenLine,
                        std::uint32_t tokenColumn) {
    tokens.push_back(Token{kind, std::move(value), tokenLine, tokenColumn});
  };

  while (i < text.size()) {
    const char c = text[i];

    if (c == '\0') {
      addError(diagnostics, fileLabel, line, column, DbcErrorCode::DbcSyntax,
               "NUL byte in DBC text");
      return false;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(1);
      continue;
    }
    // A lexical token count cap: a pathological file cannot make us build an
    // unbounded token vector before the parser ever sees it (invariant D-12).
    if (tokens.size() >= limits.maxStatements * 16u) {
      addError(diagnostics, fileLabel, line, column, DbcErrorCode::DbcLimit,
               "max_statements exceeded while tokenizing");
      return false;
    }

    const std::uint32_t startLine = line;
    const std::uint32_t startColumn = column;

    if (isIdentStart(c)) {
      std::size_t length = 0;
      while (i + length < text.size() && isIdentChar(text[i + length])) ++length;
      if (length > limits.maxTokenBytes) {
        addError(diagnostics, fileLabel, startLine, startColumn, DbcErrorCode::DbcLimit,
                 "max_token_bytes exceeded: identifier token is too long");
        return false;
      }
      push(TokenKind::Identifier, std::string(text.substr(i, length)), startLine, startColumn);
      advance(length);
      continue;
    }

    if (isDigit(c)) {
      std::size_t length = 0;
      while (i + length < text.size() && isDigit(text[i + length])) ++length;
      if (i + length < text.size() && text[i + length] == '.') {
        ++length;
        while (i + length < text.size() && isDigit(text[i + length])) ++length;
      }
      if (i + length < text.size() && (text[i + length] == 'e' || text[i + length] == 'E')) {
        std::size_t exponent = length + 1;
        if (i + exponent < text.size() && (text[i + exponent] == '+' || text[i + exponent] == '-')) {
          ++exponent;
        }
        if (i + exponent < text.size() && isDigit(text[i + exponent])) {
          while (i + exponent < text.size() && isDigit(text[i + exponent])) ++exponent;
          length = exponent;
        }
      }
      if (length > limits.maxTokenBytes) {
        addError(diagnostics, fileLabel, startLine, startColumn, DbcErrorCode::DbcLimit,
                 "max_token_bytes exceeded: numeric token is too long");
        return false;
      }
      push(TokenKind::Number, std::string(text.substr(i, length)), startLine, startColumn);
      advance(length);
      continue;
    }

    if (c == '"') {
      std::size_t length = 1;
      bool terminated = false;
      while (i + length < text.size()) {
        const char inner = text[i + length];
        if (inner == '\0') break;
        if (inner == '"') {
          terminated = true;
          break;
        }
        ++length;
        if (length > limits.maxTokenBytes) {
          addError(diagnostics, fileLabel, startLine, startColumn, DbcErrorCode::DbcLimit,
                   "max_token_bytes exceeded: string token is too long");
          return false;
        }
      }
      if (!terminated) {
        addError(diagnostics, fileLabel, startLine, startColumn, DbcErrorCode::DbcSyntax,
                 "unterminated string literal");
        return false;
      }
      push(TokenKind::String, std::string(text.substr(i + 1, length - 1)), startLine, startColumn);
      advance(length + 1);
      continue;
    }

    TokenKind kind = TokenKind::End;
    switch (c) {
      case ':': kind = TokenKind::Colon; break;
      case '|': kind = TokenKind::Pipe; break;
      case '@': kind = TokenKind::At; break;
      case '+': kind = TokenKind::Plus; break;
      case '-': kind = TokenKind::Minus; break;
      case '(': kind = TokenKind::LParen; break;
      case ')': kind = TokenKind::RParen; break;
      case '[': kind = TokenKind::LBracket; break;
      case ']': kind = TokenKind::RBracket; break;
      case ',': kind = TokenKind::Comma; break;
      case ';': kind = TokenKind::Semicolon; break;
      default: break;
    }
    if (kind == TokenKind::End) {
      addError(diagnostics, fileLabel, startLine, startColumn, DbcErrorCode::DbcSyntax,
               std::string("unexpected character in DBC text: '") + c + "'");
      return false;
    }
    push(kind, std::string(1, c), startLine, startColumn);
    advance(1);
  }

  tokens.push_back(Token{TokenKind::End, std::string(), line, column});
  return true;
}

}  // namespace tcu::dbc::detail
