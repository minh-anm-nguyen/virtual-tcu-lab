// T019: one case per validation class of contracts/dbc_subset.md section 4.
// Each asserts the CODE and the LINE: a diagnostic nobody can navigate to is
// not a diagnostic (invariant D-01, B-18).

#include <gtest/gtest.h>

#include <string>

#include "tcu/dbc/dbc_parser.hpp"

namespace {

using tcu::dbc::DbcErrorCode;

tcu::dbc::LoadResult load(const std::string& text) {
  return tcu::dbc::loadDbcBytes(text, "test.dbc");
}

/// Wraps signal lines into a one-message file. Signal N lands on line 2 + N.
std::string messageWith(const std::string& signals, const char* header = "BO_ 256 M: 8 ECU") {
  return std::string("VERSION \"lab\"\n") + header + "\n" + signals;
}

void expectCodeAtLine(const tcu::dbc::LoadResult& result, DbcErrorCode code,
                      std::uint32_t line) {
  ASSERT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(code);
  ASSERT_NE(diag, nullptr) << "expected " << tcu::dbc::toString(code) << ", got "
                           << (result.diagnostics.empty()
                                   ? "nothing"
                                   : tcu::dbc::toString(result.diagnostics[0].code));
  EXPECT_EQ(diag->line, line) << diag->message;
}

TEST(Validator_RejectsBitsOutsidePayload, OutOfBounds) {
  expectCodeAtLine(load(messageWith(" SG_ S : 56|16@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcOutOfBounds, 3);
}

TEST(Validator_RejectsOverlapInSameBranch, Overlap) {
  expectCodeAtLine(load(messageWith(
                       " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" TCU\n"
                       " SG_ A m1 : 16|16@1+ (1,0) [0|1] \"\" TCU\n"
                       " SG_ B m1 : 24|16@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcOverlap, 5);
}

TEST(Validator_AcceptsOverlapAcrossBranches, MutuallyExclusiveIsFine) {
  const auto result = load(messageWith(
      " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" TCU\n"
      " SG_ A m1 : 16|16@1+ (1,0) [0|1] \"\" TCU\n"
      " SG_ B m2 : 16|16@1+ (1,0) [0|1] \"\" TCU\n"));
  ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
}

TEST(Validator_RejectsOverlapWithUnconditional, Overlap) {
  expectCodeAtLine(load(messageWith(
                       " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" TCU\n"
                       " SG_ Always : 16|16@1+ (1,0) [0|1] \"\" TCU\n"
                       " SG_ Branch m1 : 16|16@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcOverlap, 5);
}

TEST(Validator_RejectsDuplicateMessageKey, SameFormatAndId) {
  expectCodeAtLine(load("VERSION \"lab\"\n"
                        "BO_ 256 A: 8 ECU\n"
                        " SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n"
                        "BO_ 256 B: 8 ECU\n"
                        " SG_ T : 0|8@1+ (1,0) [0|1] \"\" TCU\n"),
                   DbcErrorCode::DbcMessage, 4);
}

TEST(Validator_AcceptsStdAndEffSameNumber, TwoDistinctMessages) {
  // 2147483904 is 0x80000100: the DBC extended flag plus 0x100. FR-07, B-07.
  const auto result = load("VERSION \"lab\"\n"
                           "BO_ 256 STD: 8 ECU\n"
                           " SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n"
                           "BO_ 2147483904 EFF: 8 ECU\n"
                           " SG_ T : 0|8@1+ (1,0) [0|1] \"\" TCU\n");
  ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  EXPECT_EQ(result.db->stats().messages, 2u);
  EXPECT_NE(result.db->find(0, tcu::dbc::FrameFormat::Standard11, 256), nullptr);
  EXPECT_NE(result.db->find(0, tcu::dbc::FrameFormat::Extended29, 256), nullptr);
}

TEST(Validator_RejectsMessageIdOutOfRange, ReservedBitsSet) {
  expectCodeAtLine(load("VERSION \"lab\"\nBO_ 4026531840 X: 8 ECU\n"),
                   DbcErrorCode::DbcMessage, 2);
}

TEST(Validator_RejectsMessageIdOutOfRange, StandardIdAbove0x7FF) {
  expectCodeAtLine(load("VERSION \"lab\"\nBO_ 2048 X: 8 ECU\n"),
                   DbcErrorCode::DbcMessage, 2);
}

TEST(Validator_RejectsMessageTooLong, NineBytesInV1) {
  expectCodeAtLine(load("VERSION \"lab\"\nBO_ 256 X: 9 ECU\n"),
                   DbcErrorCode::DbcMessage, 2);
}

TEST(Validator_RejectsDuplicateSignalName, WithinOneMessage) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n"
                                    " SG_ S : 8|8@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcSignal, 4);
}

TEST(Validator_RejectsWidthZeroAndWidth65, BothAreSignalErrors) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|0@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcSignal, 3);
  expectCodeAtLine(load(messageWith(" SG_ S : 0|65@1+ (1,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcSignal, 3);
}

TEST(Validator_RejectsNonFiniteFactor, DbcRange) {
  // 1e999 overflows a double to infinity; B-17 requires the file to be rejected.
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (1e999,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcRange, 3);
}

TEST(Validator_RejectsZeroFactor, DbcRange) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (0,0) [0|1] \"\" TCU\n")),
                   DbcErrorCode::DbcRange, 3);
}

TEST(Validator_RejectsMinGreaterThanMax, DbcRange) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (1,0) [10|1] \"\" TCU\n")),
                   DbcErrorCode::DbcRange, 3);
}

TEST(Validator_RejectsTwoSelectors, DbcMux) {
  expectCodeAtLine(load(messageWith(" SG_ P1 M : 0|8@1+ (1,0) [0|255] \"\" TCU\n"
                                    " SG_ P2 M : 8|8@1+ (1,0) [0|255] \"\" TCU\n")),
                   DbcErrorCode::DbcMux, 4);
}

TEST(Validator_RejectsBranchWithoutSelector, DbcMux) {
  expectCodeAtLine(load(messageWith(" SG_ B m1 : 0|8@1+ (1,0) [0|255] \"\" TCU\n")),
                   DbcErrorCode::DbcMux, 3);
}

TEST(Validator_RejectsDuplicateEnumNumber, DbcEnum) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n") +
                        "VAL_ 256 S 0 \"Off\" 0 \"Zero\" ;\n"),
                   DbcErrorCode::DbcEnum, 4);
}

TEST(Validator_RejectsEnumForUnknownSignal, DbcEnum) {
  expectCodeAtLine(load(messageWith(" SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n") +
                        "VAL_ 256 NoSuchSignal 0 \"Off\" ;\n"),
                   DbcErrorCode::DbcEnum, 4);
}

TEST(Validator_EnforcesLimits, TooManyMessages) {
  tcu::dbc::ParseLimits limits;
  limits.maxMessages = 2;
  std::string text = "VERSION \"lab\"\n";
  for (int i = 0; i < 4; ++i) {
    text += "BO_ " + std::to_string(256 + i) + " M" + std::to_string(i) + ": 8 ECU\n";
    text += " SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n";
  }
  const auto result = tcu::dbc::loadDbcBytes(text, "test.dbc", limits);
  ASSERT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(DbcErrorCode::DbcLimit);
  ASSERT_NE(diag, nullptr);
  EXPECT_NE(diag->message.find("max_messages"), std::string::npos) << diag->message;
}

TEST(Validator_EnforcesLimits, TooManySignalsPerMessage) {
  tcu::dbc::ParseLimits limits;
  limits.maxSignalsPerMessage = 2;
  std::string signals;
  for (int i = 0; i < 4; ++i) {
    signals += " SG_ S" + std::to_string(i) + " : " + std::to_string(i * 8) +
               "|8@1+ (1,0) [0|1] \"\" TCU\n";
  }
  const auto result = tcu::dbc::loadDbcBytes(messageWith(signals), "test.dbc", limits);
  ASSERT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(DbcErrorCode::DbcLimit);
  ASSERT_NE(diag, nullptr);
  EXPECT_NE(diag->message.find("max_signals_per_message"), std::string::npos) << diag->message;
}

TEST(Validator_EnforcesLimits, FileTooLarge) {
  tcu::dbc::ParseLimits limits;
  limits.dbcMaxBytes = 32;
  const auto result = tcu::dbc::loadDbcBytes(
      messageWith(" SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n"), "test.dbc", limits);
  ASSERT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(DbcErrorCode::DbcLimit);
  ASSERT_NE(diag, nullptr);
  EXPECT_NE(diag->message.find("dbc_max_bytes"), std::string::npos) << diag->message;
}

}  // namespace
