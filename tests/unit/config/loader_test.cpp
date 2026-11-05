// Stages 1 to 4 of the pipeline: BC-14, BC-15, BC-28 and the parse rules of
// contracts/config_schema.md section 3. Every fixture here is written into a
// unique temp directory and removed again, so the cases are order-independent.

#include <gtest/gtest.h>

#include <string>

#include "loader.hpp"
#include "support/temp_config_dir.hpp"

namespace tcu::config::internal {
namespace {

using tcu::config::test::TempConfigDir;

constexpr std::size_t kOneMiB = 1024 * 1024;

std::string baseline() { return TempConfigDir::readFixture("tcu.json"); }

TEST(Loader, ParsesSampleConfig) {
  TempConfigDir dir;
  dir.writeAtomic(baseline());

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  ASSERT_EQ(outcome.error, LoadError::None) << outcome.reason;
  EXPECT_TRUE(outcome.reason.empty());
  EXPECT_TRUE(outcome.document.has("/connectivity/retry_base_ms"));
  EXPECT_TRUE(outcome.document.has("/network/tls_required"));
  std::int64_t base = 0;
  EXPECT_TRUE(outcome.document.getInt("/connectivity/retry_base_ms", base));
  EXPECT_EQ(base, 1000);
}

// BC-15: the size is probed first, so the parser is never entered at all.
TEST(Loader, RejectsOversizeBeforeParse) {
  TempConfigDir dir;
  dir.writeAtomic(baseline());

  const LoadOutcome outcome = loadFile(dir.path(), 16);

  EXPECT_EQ(outcome.error, LoadError::TooLarge);
  EXPECT_NE(outcome.reason.find("file exceeds"), std::string::npos);
  EXPECT_FALSE(outcome.document.has("/schema_version"))
      << "nothing may have been parsed";
}

TEST(Loader, MissingFileIsFileNotFound) {
  TempConfigDir dir;
  const LoadOutcome outcome = loadFile(dir.path("absent.json"), kOneMiB);
  EXPECT_EQ(outcome.error, LoadError::FileNotFound);
  EXPECT_NE(outcome.reason.find("cannot read"), std::string::npos);
}

// Section 9 rule 2. The reason names the pointer, never the value.
TEST(Loader, RejectsDuplicateKey) {
  TempConfigDir dir;
  dir.writeAtomic(R"({"schema_version": 1, "telemetry": {"period_ms": 500, "period_ms": 900}})");

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  EXPECT_EQ(outcome.error, LoadError::ParseError);
  EXPECT_NE(outcome.reason.find("duplicate key"), std::string::npos);
  EXPECT_NE(outcome.reason.find("/telemetry/period_ms"), std::string::npos);
  EXPECT_EQ(outcome.reason.find("900"), std::string::npos)
      << "a reason must not quote input (research R-10)";
}

TEST(Loader, RejectsDepthOverEight) {
  TempConfigDir dir;
  dir.writeAtomic(
      R"({"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":{"i":{"j":1}}}}}}}}}})");

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);
  EXPECT_EQ(outcome.error, LoadError::ParseError);
}

// BC-14: a file caught mid-write is rejected, which is what keeps the snapshot
// in force untouched when a writer does not use temp-file plus rename.
TEST(Loader, RejectsTruncatedFile) {
  TempConfigDir dir;
  dir.writeRaw(R"({"schema_version": 1, "connectivity": {"retry_base_ms": 10)");

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  EXPECT_EQ(outcome.error, LoadError::ParseError);
  EXPECT_NE(outcome.reason.find("parse error at byte offset"), std::string::npos);
}

TEST(Loader, RejectsTrailingDataAndComments) {
  TempConfigDir dir;

  dir.writeAtomic(R"({"schema_version": 1} {"extra": true})");
  EXPECT_EQ(loadFile(dir.path(), kOneMiB).error, LoadError::ParseError);

  dir.writeAtomic("{\n  // a comment\n  \"schema_version\": 1\n}");
  EXPECT_EQ(loadFile(dir.path(), kOneMiB).error, LoadError::ParseError);
}

TEST(Loader, RejectsInvalidUtf8) {
  TempConfigDir dir;
  std::string text = R"({"schema_version": 1, "network": {"cloud_endpoint": "x"}})";
  // One stray continuation byte is enough: strict JSON is UTF-8 only.
  text.insert(text.find("x"), 1, static_cast<char>(0x80));
  dir.writeAtomic(text);

  EXPECT_EQ(loadFile(dir.path(), kOneMiB).error, LoadError::ParseError);
}

TEST(Loader, RejectsNonObjectRoot) {
  TempConfigDir dir;
  dir.writeAtomic("[1, 2, 3]");
  EXPECT_EQ(loadFile(dir.path(), kOneMiB).error, LoadError::SchemaError);
}

// BC-28: a version the binary does not understand is refused outright. No
// blind parsing of fields that may have changed meaning.
TEST(Loader, RejectsUnsupportedSchemaVersion) {
  TempConfigDir dir;
  dir.writeAtomic(TempConfigDir::readFixture("invalid_schema.json"));

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  EXPECT_EQ(outcome.error, LoadError::UnsupportedVersion);
  EXPECT_NE(outcome.reason.find("unsupported schema_version"), std::string::npos);
}

// A missing or mistyped version is a schema error, not an unsupported version:
// the operator action differs, so the report has to differ too.
TEST(Loader, MissingVersionIsASchemaError) {
  TempConfigDir dir;
  dir.writeAtomic(R"({"telemetry": {"period_ms": 500}})");

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  EXPECT_EQ(outcome.error, LoadError::SchemaError);
  EXPECT_NE(outcome.reason.find("missing required key /schema_version"),
            std::string::npos);
}

TEST(Loader, VersionOfWrongTypeIsASchemaError) {
  TempConfigDir dir;
  dir.writeAtomic(R"({"schema_version": "1"})");

  const LoadOutcome outcome = loadFile(dir.path(), kOneMiB);

  EXPECT_EQ(outcome.error, LoadError::SchemaError);
  EXPECT_NE(outcome.reason.find("wrong type at /schema_version"), std::string::npos);
}

}  // namespace
}  // namespace tcu::config::internal
