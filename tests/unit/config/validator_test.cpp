// Stage 5: the schema table walk and the cross-field rules. BC-02, BC-04,
// BC-05, the unknown-key policy (R-05) and the reason templates (R-10).

#include <gtest/gtest.h>

#include <string>

#include "loader.hpp"
#include "support/temp_config_dir.hpp"
#include "tcu/config/config_schema.hpp"
#include "validator.hpp"

namespace tcu::config::internal {
namespace {

using tcu::config::test::TempConfigDir;

constexpr std::size_t kOneMiB = 1024 * 1024;

// Loads a document from text, then validates it. Going through the loader keeps
// the tests honest: they exercise the same path the service uses.
ValidateOutcome validateText(const std::string& text) {
  TempConfigDir dir;
  dir.writeAtomic(text);
  LoadOutcome loaded = loadFile(dir.path(), kOneMiB);
  if (loaded.error != LoadError::None) {
    ValidateOutcome out;
    out.error = loaded.error;
    out.reason = loaded.reason;
    return out;
  }
  return validate(loaded.document, dir.path().string());
}

std::string baseline() { return TempConfigDir::readFixture("tcu.json"); }

// Replaces one JSON fragment with another. Crude on purpose: the fixtures are
// ours, the shapes are known, and a JSON writer here would be a second parser
// to keep correct.
std::string withReplacement(const std::string& from, const std::string& to) {
  std::string text = baseline();
  const std::size_t at = text.find(from);
  if (at == std::string::npos) {
    return text;
  }
  return text.replace(at, from.size(), to);
}

TEST(Validator, AcceptsTheShippedBaseline) {
  const ValidateOutcome outcome = validateText(baseline());
  ASSERT_EQ(outcome.error, LoadError::None) << outcome.reason;
  EXPECT_EQ(outcome.snapshot.connectivity.retryBaseMs, 1000);
  EXPECT_EQ(outcome.snapshot.connectivity.retryMaxMs, 30000);
  EXPECT_EQ(outcome.snapshot.telemetry.periodMs, 5000);
  EXPECT_EQ(outcome.snapshot.logging.level, tcu::logging::Severity::Info);
  EXPECT_EQ(outcome.snapshot.logging.queueCapacity, 8192);
  EXPECT_TRUE(outcome.snapshot.logging.consoleEnabled);
  EXPECT_EQ(outcome.snapshot.network.cloudEndpoint, "https://mock-cloud.local");
  EXPECT_TRUE(outcome.snapshot.network.tlsRequired);
}

// BC-02: every one of the 13 keys, each renamed in turn. None may be silently
// defaulted, because there is no defaults layer at all (research R-16).
TEST(Validator, RejectsMissingRequiredKey) {
  for (const FieldSpec& spec : kFieldSpecs) {
    const std::string pointer(spec.pointer);
    const std::size_t slash = pointer.rfind('/');
    const std::string leaf = pointer.substr(slash + 1);

    std::string text = baseline();
    const std::size_t key_at = text.find("\"" + leaf + "\"");
    ASSERT_NE(key_at, std::string::npos) << pointer;
    // Renaming makes the key both missing and unknown. Either reason is
    // correct; what matters is that the candidate is rejected.
    text.replace(key_at + 1, leaf.size(), std::string(leaf.size(), 'z'));

    const ValidateOutcome outcome = validateText(text);
    EXPECT_NE(outcome.error, LoadError::None) << "renaming " << pointer;
  }
}

TEST(Validator, RejectsWrongType) {
  const ValidateOutcome outcome = validateText(
      withReplacement("\"retry_base_ms\": 1000", "\"retry_base_ms\": \"1000\""));
  EXPECT_EQ(outcome.error, LoadError::SchemaError);
  EXPECT_NE(outcome.reason.find("wrong type at /connectivity/retry_base_ms"),
            std::string::npos);
  EXPECT_NE(outcome.reason.find("expected int"), std::string::npos);
}

// A float where the schema says int is a type error, not something to round.
TEST(Validator, RejectsFloatWhereIntIsRequired) {
  const ValidateOutcome outcome = validateText(
      withReplacement("\"period_ms\": 5000", "\"period_ms\": 5000.5"));
  EXPECT_EQ(outcome.error, LoadError::SchemaError);
  EXPECT_NE(outcome.reason.find("wrong type at /telemetry/period_ms"),
            std::string::npos);
}

// BC-04, both bounds of every numeric row.
TEST(Validator, RejectsOutOfRange) {
  struct Case {
    std::string from;
    std::string to;
    std::string pointer;
  };
  const Case cases[] = {
      {"\"retry_base_ms\": 1000", "\"retry_base_ms\": 99", "/connectivity/retry_base_ms"},
      {"\"retry_base_ms\": 1000", "\"retry_base_ms\": 60001", "/connectivity/retry_base_ms"},
      {"\"max_attempts\": 0", "\"max_attempts\": 101", "/connectivity/max_attempts"},
      {"\"max_attempts\": 0", "\"max_attempts\": -1", "/connectivity/max_attempts"},
      {"\"period_ms\": 5000", "\"period_ms\": 199", "/telemetry/period_ms"},
      {"\"period_ms\": 5000", "\"period_ms\": 3600001", "/telemetry/period_ms"},
      {"\"offline_queue_capacity\": 2048", "\"offline_queue_capacity\": 15", "/telemetry/offline_queue_capacity"},
      {"\"queue_capacity\": 8192", "\"queue_capacity\": 127", "/logging/queue_capacity"},
      {"\"queue_capacity\": 8192", "\"queue_capacity\": 65537", "/logging/queue_capacity"},
      {"\"file_max_bytes\": 10485760", "\"file_max_bytes\": 1024", "/logging/file_max_bytes"},
      {"\"file_count\": 5", "\"file_count\": 0", "/logging/file_count"},
      {"\"file_count\": 5", "\"file_count\": 21", "/logging/file_count"},
  };

  for (const Case& c : cases) {
    const ValidateOutcome outcome = validateText(withReplacement(c.from, c.to));
    EXPECT_EQ(outcome.error, LoadError::InvalidValue) << c.to;
    EXPECT_NE(outcome.reason.find("out of range at " + c.pointer),
              std::string::npos)
        << c.to << " gave " << outcome.reason;
  }
}

// BC-05: the retry invariant is a relation, so it cannot be a per-field range.
TEST(Validator, RejectsCrossFieldRetryOrder) {
  const ValidateOutcome outcome =
      validateText(TempConfigDir::readFixture("invalid_retry.json"));

  EXPECT_EQ(outcome.error, LoadError::InvalidValue);
  EXPECT_NE(outcome.reason.find("cross-field"), std::string::npos);
  EXPECT_NE(outcome.reason.find("/connectivity/retry_max_ms"), std::string::npos);
  EXPECT_NE(outcome.reason.find("/connectivity/retry_base_ms"), std::string::npos);
}

TEST(Validator, RejectsTlsFalseForNonlocalEndpoint) {
  std::string text =
      withReplacement("\"cloud_endpoint\": \"https://mock-cloud.local\"",
                      "\"cloud_endpoint\": \"https://cloud.example.com\"");
  const std::string from = "\"tls_required\": true";
  text.replace(text.find(from), from.size(), "\"tls_required\": false");

  const ValidateOutcome outcome = validateText(text);
  EXPECT_EQ(outcome.error, LoadError::InvalidValue);
  EXPECT_NE(outcome.reason.find("/network/tls_required"), std::string::npos);
}

// Loopback is the documented exception: the simulated cloud lives there.
TEST(Validator, AllowsTlsFalseForLoopback) {
  std::string text =
      withReplacement("\"cloud_endpoint\": \"https://mock-cloud.local\"",
                      "\"cloud_endpoint\": \"http://127.0.0.1:8080\"");
  const std::string from = "\"tls_required\": true";
  text.replace(text.find(from), from.size(), "\"tls_required\": false");

  EXPECT_EQ(validateText(text).error, LoadError::None);
}

TEST(Validator, RejectsBadEndpointScheme) {
  const ValidateOutcome outcome =
      validateText(withReplacement("\"cloud_endpoint\": \"https://mock-cloud.local\"",
                                   "\"cloud_endpoint\": \"ftp://mock-cloud.local\""));
  EXPECT_EQ(outcome.error, LoadError::InvalidValue);
  EXPECT_NE(outcome.reason.find("/network/cloud_endpoint"), std::string::npos);
}

TEST(Validator, RejectsUnknownKey) {
  std::string text = baseline();
  const std::size_t at = text.find("\"telemetry\"");
  text.insert(at, "\"vendor_tweak\": 7,\n  ");

  const ValidateOutcome outcome = validateText(text);
  EXPECT_EQ(outcome.error, LoadError::SchemaError);
  EXPECT_NE(outcome.reason.find("unknown key /vendor_tweak"), std::string::npos);
}

TEST(Validator, AcceptsLevelEnumNamesOnly) {
  EXPECT_EQ(
      validateText(withReplacement("\"level\": \"INFO\"", "\"level\": \"DEBUG\"")).error,
      LoadError::None);

  const ValidateOutcome bad =
      validateText(withReplacement("\"level\": \"INFO\"", "\"level\": \"info\""));
  EXPECT_EQ(bad.error, LoadError::InvalidValue);
  EXPECT_NE(bad.reason.find("/logging/level"), std::string::npos);
}

// R-10 end to end: a recognisable literal placed in a value must appear in no
// reason string at all.
TEST(Redaction, ReasonStringsNeverQuoteInput) {
  const std::string marker = "SUPERSECRETMARKER";
  const ValidateOutcome outcome = validateText(
      withReplacement("\"level\": \"INFO\"", "\"level\": \"" + marker + "\""));

  ASSERT_NE(outcome.error, LoadError::None);
  EXPECT_EQ(outcome.reason.find(marker), std::string::npos) << outcome.reason;
}

}  // namespace
}  // namespace tcu::config::internal
