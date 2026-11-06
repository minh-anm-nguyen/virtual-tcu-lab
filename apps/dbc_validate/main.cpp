// dbc_validate: print every diagnostic for a DBC file.
//
// Exit codes, per contracts/dbc_api.md section 5: 0 clean, 1 rejected,
// 2 usage or I/O error. Output is the section 5 format of
// contracts/dbc_subset.md - file:line:column: CODE: message - so an editor can
// jump to it.

#include <cstdio>
#include <string>

#include "tcu/dbc/dbc_parser.hpp"

namespace {

int usage(const char* program) {
  std::fprintf(stderr, "usage: %s <file.dbc>\n", program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return usage(argv[0]);
  const std::string path = argv[1];

  const tcu::dbc::LoadResult result = tcu::dbc::loadDbcFile(path);

  bool sourceProblem = false;
  for (const auto& diagnostic : result.diagnostics) {
    const char* severity = diagnostic.severity == tcu::dbc::Severity::Error ? "error" : "warning";
    // A warning carries no error code, so printing one would read as "NONE".
    const bool hasCode = diagnostic.code != tcu::dbc::DbcErrorCode::None;
    const char* code = hasCode ? tcu::dbc::toString(diagnostic.code) : "";
    const char* separator = hasCode ? ": " : "";
    if (diagnostic.line == 0) {
      std::fprintf(stdout, "%s: %s: %s%s%s\n", diagnostic.file.c_str(), severity, code,
                   separator, diagnostic.message.c_str());
    } else {
      std::fprintf(stdout, "%s:%u:%u: %s: %s%s%s\n", diagnostic.file.c_str(), diagnostic.line,
                   diagnostic.column, severity, code, separator, diagnostic.message.c_str());
    }
    if (diagnostic.code == tcu::dbc::DbcErrorCode::DbcSource) sourceProblem = true;
  }

  if (!result.ok()) {
    // A file we could not read at all is an I/O problem, not a rejected DBC.
    if (sourceProblem) return 2;
    std::fprintf(stdout, "%s: rejected\n", path.c_str());
    return 1;
  }

  std::fprintf(stdout, "%s: ok, version %llu, sha256 %s\n", path.c_str(),
               static_cast<unsigned long long>(result.db->version()), result.db->hash().c_str());
  std::fprintf(stdout, "  messages %zu, signals %zu, ignored statements %zu\n",
               result.db->stats().messages, result.db->stats().signals,
               result.db->stats().ignoredStatements);
  return 0;
}
