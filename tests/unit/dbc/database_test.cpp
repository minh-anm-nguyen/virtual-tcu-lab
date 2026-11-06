// T020: the compiled database is immutable, versioned, hashed, and keyed by
// (bus, format, id) - invariants P-07..P-10.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

#include "tcu/dbc/dbc_parser.hpp"

namespace {

using tcu::dbc::FrameFormat;

std::string fixturePath(const std::string& relative) {
  return std::string(TCU_DBC_FIXTURE_DIR) + "/" + relative;
}

std::string readFixture(const std::string& relative) {
  std::ifstream in(fixturePath(relative), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(Database_VersionAndHashPublished, HashIsTheDigestOfTheBytes) {
  const std::string text = readFixture("virtual_tcu.dbc");
  const auto a = tcu::dbc::loadDbcBytes(text, "a.dbc");
  const auto b = tcu::dbc::loadDbcBytes(text, "b.dbc");
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.db->version(), 1u);
  EXPECT_EQ(a.db->hash().size(), 64u);
  EXPECT_EQ(a.db->hash(), b.db->hash()) << "identical bytes must hash alike";

  std::string changed = text;
  changed.back() = changed.back() == '\n' ? ' ' : '\n';
  const auto c = tcu::dbc::loadDbcBytes(changed, "c.dbc");
  ASSERT_TRUE(c.ok());
  EXPECT_NE(a.db->hash(), c.db->hash()) << "a one-byte change must change the hash";
}

TEST(Database_VersionAndHashPublished, StatsCountWhatWasLoaded) {
  const auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.db->stats().messages, 3u);
  EXPECT_EQ(result.db->stats().signals, 8u);
  EXPECT_EQ(result.db->messages().size(), 3u);
}

TEST(Database_IsImmutable, EveryAccessorHandsBackConst) {
  // A compile-time check: if find() ever returned a mutable pointer, or the
  // database exposed a non-const accessor, this would stop compiling. That is
  // the point - invariant P-07 is structural, not a review rule.
  using Loaded = decltype(tcu::dbc::loadDbcBytes("", "").db);
  static_assert(std::is_same<Loaded, std::shared_ptr<const tcu::dbc::CompiledDbc>>::value,
                "a loaded database must only be reachable as shared_ptr<const CompiledDbc>");
  using Found = decltype(std::declval<const tcu::dbc::CompiledDbc&>().find(
      0, FrameFormat::Standard11, 0));
  static_assert(std::is_same<Found, const tcu::dbc::CompiledMessage*>::value,
                "find() must hand back a const message");
  SUCCEED();
}

TEST(Database_LookupDistinguishesFormat, StandardAndExtendedAreSeparateEntries) {
  const auto result = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 256 STD: 8 ECU\n"
      " SG_ S : 0|8@1+ (1,0) [0|1] \"\" TCU\n"
      "BO_ 2147483904 EFF: 8 ECU\n"
      " SG_ T : 0|8@1+ (1,0) [0|1] \"\" TCU\n",
      "both.dbc");
  ASSERT_TRUE(result.ok());
  const auto* standard = result.db->find(0, FrameFormat::Standard11, 256);
  const auto* extended = result.db->find(0, FrameFormat::Extended29, 256);
  ASSERT_NE(standard, nullptr);
  ASSERT_NE(extended, nullptr);
  EXPECT_EQ(standard->name, "STD");
  EXPECT_EQ(extended->name, "EFF");
  EXPECT_NE(standard, extended);
}

TEST(Database_LookupDistinguishesFormat, UnknownIdentifierIsNull) {
  const auto result = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "virtual_tcu.dbc");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.db->find(0, FrameFormat::Standard11, 0x7FF), nullptr);
  EXPECT_EQ(result.db->find(0, FrameFormat::Extended29, 256), nullptr)
      << "the standard 256 must not answer an extended lookup";
}

TEST(Database_SemanticHashStable, SameFileSameHashes) {
  const std::string text = readFixture("virtual_tcu.dbc");
  const auto a = tcu::dbc::loadDbcBytes(text, "a.dbc");
  const auto b = tcu::dbc::loadDbcBytes(text, "b.dbc");
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  const auto* left = a.db->findSignal(0, FrameFormat::Standard11, 256, "VehicleSpeed");
  const auto* right = b.db->findSignal(0, FrameFormat::Standard11, 256, "VehicleSpeed");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->semanticHash, right->semanticHash);
}

TEST(Database_SemanticHashStable, ChangingFactorChangesOnlyThatSignal) {
  const char* before =
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ A : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ B : 16|8@1+ (1,0) [0|1] \"\" TCU\n";
  const char* after =
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ A : 0|16@1+ (0.5,0) [0|250] \"km/h\" TCU\n"
      " SG_ B : 16|8@1+ (1,0) [0|1] \"\" TCU\n";
  const auto one = tcu::dbc::loadDbcBytes(before, "one.dbc");
  const auto two = tcu::dbc::loadDbcBytes(after, "two.dbc");
  ASSERT_TRUE(one.ok());
  ASSERT_TRUE(two.ok());

  const auto* aBefore = one.db->findSignal(0, FrameFormat::Standard11, 256, "A");
  const auto* aAfter = two.db->findSignal(0, FrameFormat::Standard11, 256, "A");
  const auto* bBefore = one.db->findSignal(0, FrameFormat::Standard11, 256, "B");
  const auto* bAfter = two.db->findSignal(0, FrameFormat::Standard11, 256, "B");
  ASSERT_NE(aBefore, nullptr);
  ASSERT_NE(aAfter, nullptr);
  ASSERT_NE(bBefore, nullptr);
  ASSERT_NE(bAfter, nullptr);

  // A changed physical meaning, so its cache entry must be reset on reload
  // (research R-09); B did not, so its last-good may be kept.
  EXPECT_NE(aBefore->semanticHash, aAfter->semanticHash);
  EXPECT_EQ(bBefore->semanticHash, bAfter->semanticHash);
}

TEST(Database_RejectsSymlinkAndDirectory, DirectoryIsASourceError) {
  const auto result = tcu::dbc::loadDbcFile(std::string(TCU_DBC_FIXTURE_DIR));
  EXPECT_FALSE(result.ok());
  ASSERT_NE(result.firstWithCode(tcu::dbc::DbcErrorCode::DbcSource), nullptr);
}

TEST(Database_RejectsSymlinkAndDirectory, MissingFileIsASourceError) {
  const auto result = tcu::dbc::loadDbcFile(fixturePath("no_such_file.dbc"));
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.firstWithCode(tcu::dbc::DbcErrorCode::DbcSource), nullptr);
}

TEST(Database_RejectsSymlinkAndDirectory, RealFileLoadsAndNamesThePathAsGiven) {
  const std::string path = fixturePath("virtual_tcu.dbc");
  const auto result = tcu::dbc::loadDbcFile(path);
  ASSERT_TRUE(result.ok()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  EXPECT_EQ(result.db->sourcePath(), path);
  // The hash must match the bytes on disk, not the parsed model.
  const auto fromBytes = tcu::dbc::loadDbcBytes(readFixture("virtual_tcu.dbc"), "x.dbc");
  ASSERT_TRUE(fromBytes.ok());
  EXPECT_EQ(result.db->hash(), fromBytes.db->hash());
}

TEST(Database_EnforcesFileSizeBeforeReading, LimitHitOnDisk) {
  tcu::dbc::ParseLimits limits;
  limits.dbcMaxBytes = 8;
  const auto result = tcu::dbc::loadDbcFile(fixturePath("virtual_tcu.dbc"), limits);
  EXPECT_FALSE(result.ok());
  const auto* diag = result.firstWithCode(tcu::dbc::DbcErrorCode::DbcLimit);
  ASSERT_NE(diag, nullptr);
  EXPECT_NE(diag->message.find("dbc_max_bytes"), std::string::npos) << diag->message;
}

}  // namespace
