// T017/T018: the lexer's locations and the parser's three-tier handling of
// supported / ignorable / rejected statements (contracts/dbc_subset.md 1-3).

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "tcu/dbc/dbc_parser.hpp"

namespace {

using tcu::dbc::DbcErrorCode;
using tcu::dbc::Severity;

std::string readFixture(const std::string& relative) {
  std::ifstream in(std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

tcu::dbc::LoadResult load(const std::string& text) {
  return tcu::dbc::loadDbcBytes(text, "test.dbc");
}

const char* kValid = R"(VERSION "lab"
BU_: ECU TCU

BO_ 256 VEHICLE_STATUS: 8 ECU
 SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] "km/h" TCU
 SG_ Ignition : 16|1@1+ (1,0) [0|1] "" TCU
)";

TEST(Parser_AcceptsSampleSubset, LabFixtureLoads) {
  const auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  EXPECT_EQ(result.db->stats().messages, 3u);
  EXPECT_EQ(result.db->stats().signals, 8u);
  EXPECT_EQ(result.db->version(), 1u);
  EXPECT_EQ(result.db->hash().size(), 64u);
}

TEST(Parser_AcceptsSampleSubset, EnumIsAttachedToItsSignal) {
  const auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(result.ok());
  const auto* meta = result.db->findSignal(0, tcu::dbc::FrameFormat::Standard11, 256, "Ignition");
  ASSERT_NE(meta, nullptr);
  ASSERT_EQ(meta->valueDescriptions.size(), 2u);
  EXPECT_EQ(meta->valueDescriptions[0].second, "Off");
  EXPECT_EQ(meta->valueDescriptions[1].second, "On");
}

TEST(Lexer_ReportsLineAndColumn, PointsAtTheOffendingToken) {
  // The bogus token sits on line 4, column 1.
  const std::string text = std::string(kValid) + "\nGARBAGE_ 1\n";
  const auto result = load(text);
  EXPECT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(DbcErrorCode::DbcSyntax);
  ASSERT_NE(diag, nullptr);
  EXPECT_EQ(diag->line, 8u);
  EXPECT_EQ(diag->column, 1u);
}

TEST(Lexer_RejectsNulByte, AnywhereInTheFile) {
  std::string text = kValid;
  text.push_back('\0');
  text.append("BO_ 300 X: 8 ECU\n");
  const auto result = load(text);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::DbcSyntax), nullptr);
}

TEST(Lexer_RejectsOverlongToken, NamesTheLimit) {
  tcu::dbc::ParseLimits limits;
  limits.maxTokenBytes = 16;
  const std::string text = std::string(kValid) + "BO_ 300 " + std::string(64, 'X') + ": 8 ECU\n";
  const auto result = tcu::dbc::loadDbcBytes(text, "test.dbc", limits);
  EXPECT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(DbcErrorCode::DbcLimit);
  ASSERT_NE(diag, nullptr);
  EXPECT_NE(diag->message.find("token"), std::string::npos);
}

TEST(Lexer_HandlesCrLf, SameResultAsLf) {
  std::string crlf;
  for (const char* p = kValid; *p != '\0'; ++p) {
    if (*p == '\n') crlf.push_back('\r');
    crlf.push_back(*p);
  }
  const auto lf = load(kValid);
  const auto cr = load(crlf);
  ASSERT_TRUE(lf.ok());
  ASSERT_TRUE(cr.ok()) << (cr.diagnostics.empty() ? "" : cr.diagnostics[0].message);
  EXPECT_EQ(lf.db->stats().signals, cr.db->stats().signals);
}

TEST(Parser_IgnoresAllowlistedStatements, LoadsWithWarningsNotErrors) {
  const auto result =
      tcu::dbc::loadDbcBytes(readFixture("invalid/ignorable_statements.dbc"), "ignorable.dbc");
  ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  EXPECT_GT(result.db->stats().ignoredStatements, 0u);
  for (const auto& diag : result.diagnostics) {
    EXPECT_EQ(diag.severity, Severity::Warning) << diag.message;
  }
}

TEST(Parser_RejectsSignalBeforeMessage, IsASyntaxError) {
  const auto result = load("VERSION \"lab\"\n SG_ Orphan : 0|8@1+ (1,0) [0|1] \"\" TCU\n");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::DbcSyntax), nullptr);
}

TEST(Parser_RejectsSigValtype, UnsupportedFeature) {
  const auto result = load(std::string(kValid) + "SIG_VALTYPE_ 256 VehicleSpeed : 1;\n");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::UnsupportedFeature), nullptr);
}

TEST(Parser_RejectsExtendedMux, UnsupportedFeature) {
  const auto result = load(std::string(kValid) + "SG_MUL_VAL_ 256 VehicleSpeed Ignition 1-2;\n");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::UnsupportedFeature), nullptr);
}

TEST(Parser_RejectsUnknownDirective, NotSilentlySkipped) {
  const auto result = load(std::string(kValid) + "WHAT_IS_THIS_ 1 2 3;\n");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::DbcSyntax), nullptr);
}

TEST(Parser_RejectsValTableReference, DbcEnum) {
  const auto result = load(std::string(kValid) + "VAL_ 256 Ignition SomeTableName ;\n");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(DbcErrorCode::DbcEnum), nullptr);
}

TEST(Parser_ReportsAllDiagnostics, NotOnlyTheFirst) {
  const std::string text =
      "VERSION \"lab\"\n"
      "BO_ 256 A: 8 ECU\n"
      " SG_ S1 : 0|0@1+ (1,0) [0|1] \"\" TCU\n"          // width 0
      " SG_ S2 : 0|8@1+ (1,0) [5|1] \"\" TCU\n"          // min > max
      "BO_ 999999999 B: 9 ECU\n";                        // id + length
  const auto result = load(text);
  EXPECT_FALSE(result.ok());
  EXPECT_GE(result.diagnostics.size(), 3u);
  for (std::size_t i = 1; i < result.diagnostics.size(); ++i) {
    const auto& previous = result.diagnostics[i - 1];
    const auto& current = result.diagnostics[i];
    EXPECT_TRUE(previous.line < current.line ||
                (previous.line == current.line && previous.column <= current.column))
        << "diagnostics are not ordered by (line, column)";
  }
}

TEST(Parser_NeverThrows, ForArbitraryBytes) {
  const std::string inputs[] = {
      "", "\n\n\n", "BO_", "BO_ ", "BO_ 1", "SG_ : |@+ (,) [|] \"\"",
      "VERSION", "VAL_ ;", std::string(1000, '{'), std::string(100, '\xff'),
      "BO_ 256 A: 8 ECU\n SG_ S : 0|64@0- (1e999,0) [0|0] \"\" TCU\n",
  };
  for (const auto& input : inputs) {
    EXPECT_NO_THROW({ (void)load(input); }) << "input: " << input.substr(0, 20);
  }
}

}  // namespace
