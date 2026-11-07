// T054: the two CLIs, through their real command line.
//
// Exit codes are a contract (contracts/dbc_api.md section 5): 0 clean, 1
// rejected or a decode error, 2 usage or I/O. And the output must be
// byte-identical for identical input - no wall clock, no map iteration order,
// no locale (NFR-09). Both were checked by hand during implementation, which is
// exactly the kind of check that rots without a test.

#include <gtest/gtest.h>

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <string>

namespace {

struct CliResult {
  int status{-1};
  std::string output;
};

/// Runs a command, capturing stdout (stderr merged) and the exit status.
/// Not named Run: inside a TEST body that resolves to testing::Test::Run().
CliResult runCli(const std::string& command) {
  CliResult result;
  std::FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (pipe == nullptr) return result;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }
  const int closed = pclose(pipe);
  result.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
  return result;
}

std::string fixture(const std::string& relative) {
  return std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative;
}

const char* validate() { return TCU_DBC_VALIDATE_BIN; }
const char* decode() { return TCU_DBC_DECODE_BIN; }

TEST(Cli_ValidateExitCodes, ZeroForAValidFile) {
  const CliResult result = runCli(std::string(validate()) + " " + fixture("virtual_tcu.dbc"));
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_NE(result.output.find("ok, version 1"), std::string::npos) << result.output;
  // The hash is part of the output, so a schema change is visible in a diff.
  EXPECT_NE(result.output.find("sha256 "), std::string::npos) << result.output;
}

TEST(Cli_ValidateExitCodes, OneForARejectedFile) {
  const CliResult result =
      runCli(std::string(validate()) + " " + fixture("invalid/overlap_unconditional.dbc"));
  EXPECT_EQ(result.status, 1) << result.output;
  EXPECT_NE(result.output.find("DBC_OVERLAP"), std::string::npos) << result.output;
  // Navigable: file:line:column must be in the line.
  EXPECT_NE(result.output.find(".dbc:4:"), std::string::npos) << result.output;
}

TEST(Cli_ValidateExitCodes, TwoForAMissingFileAndForBadUsage) {
  const CliResult missing = runCli(std::string(validate()) + " " + fixture("no_such_file.dbc"));
  EXPECT_EQ(missing.status, 2) << missing.output;
  EXPECT_NE(missing.output.find("DBC_SOURCE"), std::string::npos) << missing.output;

  const CliResult usage = runCli(std::string(validate()));
  EXPECT_EQ(usage.status, 2) << usage.output;
  EXPECT_NE(usage.output.find("usage:"), std::string::npos) << usage.output;
}

TEST(Cli_ValidateExitCodes, ZeroWithWarningsForIgnorableStatements) {
  // A file full of CM_/BA_/VAL_TABLE_ loads: those are warnings, not errors.
  const CliResult result =
      runCli(std::string(validate()) + " " + fixture("invalid/ignorable_statements.dbc"));
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_NE(result.output.find("warning"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("NONE"), std::string::npos)
      << "a warning must not print an empty error code";
}

TEST(Cli_DecodeOutputStable, SpecSection73ValuesAndByteIdenticalTwice) {
  const std::string command = std::string(decode()) + " --dbc " + fixture("virtual_tcu.dbc") +
                              " --hex 100:1027010050000000" +
                              " --hex 101:ABC0000000000000" +
                              " --hex 102:017FD20400000000" +
                              " --hex 102:027F9CFF00000000";
  const CliResult first = runCli(command);
  ASSERT_EQ(first.status, 0) << first.output;

  // The documented demo output of spec section 20.2.
  EXPECT_NE(first.output.find("physical=100 km/h"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("[On]"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("physical=-40 degC"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("signed=-1348"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("physical=-674 Nm"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("physical=12.34 V"), std::string::npos) << first.output;
  EXPECT_NE(first.output.find("physical=-10 degC"), std::string::npos) << first.output;

  const CliResult second = runCli(command);
  EXPECT_EQ(second.status, 0);
  EXPECT_EQ(first.output, second.output) << "the CLI is not deterministic for identical input";
}

TEST(Cli_DecodeOutputStable, OneWhenAFrameFails) {
  // A short payload and an unknown identifier: both are reported, exit is 1.
  const CliResult result = runCli(std::string(decode()) + " --dbc " + fixture("virtual_tcu.dbc") +
                         " --hex 100:102701 --hex 999:0000000000000000");
  EXPECT_EQ(result.status, 1) << result.output;
  EXPECT_NE(result.output.find("wrong_length"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("unknown_id"), std::string::npos) << result.output;
}

TEST(Cli_DecodeOutputStable, CandumpCaptureEndToEnd) {
  // T058's fixture, including one unparsable line that must be skipped and
  // counted rather than guessed at, and one unknown identifier.
  const CliResult result = runCli(std::string(decode()) + " --dbc " + fixture("virtual_tcu.dbc") +
                         " --candump " + fixture("capture_normal.log"));
  EXPECT_EQ(result.status, 1) << "the capture contains an unknown id, so exit is 1";
  EXPECT_NE(result.output.find("skipped 1 unparsable line"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("physical=100 km/h"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("physical=12.34 V"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("unknown_id"), std::string::npos) << result.output;
}

TEST(Cli_DecodeOutputStable, JsonCarriesTheSchemaIdentity) {
  const CliResult result = runCli(std::string(decode()) + " --dbc " + fixture("virtual_tcu.dbc") +
                         " --hex 100:1027010050000000 --json");
  ASSERT_EQ(result.status, 0) << result.output;
  EXPECT_NE(result.output.find("\"dbc_version\":1"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("\"dbc_hash\":\""), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("\"quality\":\"valid\""), std::string::npos) << result.output;
}

TEST(Cli_DecodeOutputStable, TwoForUsageAndForARejectedDbc) {
  const CliResult usage = runCli(std::string(decode()) + " --dbc " + fixture("virtual_tcu.dbc"));
  EXPECT_EQ(usage.status, 2) << "no --hex and no --candump is a usage error";

  const CliResult rejected = runCli(std::string(decode()) + " --dbc " +
                           fixture("invalid/overlap_unconditional.dbc") + " --hex 100:0000");
  EXPECT_EQ(rejected.status, 2) << rejected.output;
  EXPECT_NE(rejected.output.find("DBC_OVERLAP"), std::string::npos) << rejected.output;
}

}  // namespace
