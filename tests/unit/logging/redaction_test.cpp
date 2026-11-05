// BC-19 and research R-11. Redaction happens twice, at ingestion and again at
// formatting, and section 10.5 asks for both - so each stage is asserted on its
// own. Either one alone would pass BC-19, which is exactly why one test is not
// enough here.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "json_formatter.hpp"
#include "redaction.hpp"
#include "support/fake_clock.hpp"
#include "support/recording_sink.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/logging/log_types.hpp"

namespace tcu::logging {
namespace {

constexpr const char* kSecret = "s3cr3t-bearer-token-do-not-log";
constexpr const char* kVin = "VF1AAAAA555777999";

LoggerOptions testOptions(test::FakeClock& clock) {
  LoggerOptions options;
  options.minLevel = Severity::Trace;
  options.queueCapacity = 64;
  options.clock = [&clock] { return clock(); };
  return options;
}

// Stage one: the value is gone before the record is queued, so it never sits in
// memory the writer thread (or a crash dump) could reach.
TEST(Redaction, AppliedAtIngestion) {
  std::vector<Field> fields;
  fields.push_back(classified("token", kSecret, DataClass::Secret));
  fields.push_back(classified("vin", kVin, DataClass::VehicleSensitive));
  fields.push_back(f("generation", 7));

  internal::redactInPlace(fields);

  EXPECT_EQ(std::get<std::string>(fields[0].value), internal::kRedacted);
  EXPECT_EQ(std::get<std::string>(fields[1].value), internal::kRedacted);
  EXPECT_EQ(std::get<std::int64_t>(fields[2].value), 7);
}

// Stage two: a record that reached the formatter still carrying a non-public
// plaintext value - a future sink, a hand-built record - is redacted anyway.
TEST(Redaction, AppliedAtFormatting) {
  LogRecord record;
  record.timestampUtc = test::FakeClock::epoch();
  record.component = "ota";
  record.eventName = "download_start";
  record.fields.push_back(classified("token", kSecret, DataClass::Secret));

  const std::string line = internal::formatRecord(record, 16 * 1024).line;

  EXPECT_EQ(line.find(kSecret), std::string::npos)
      << "the formatter must not trust that ingestion already redacted";
  EXPECT_NE(line.find(internal::kRedacted), std::string::npos);
}

// BC-19 end to end: no sink, no counter and no error string carries the value.
TEST(Redaction, SecretAbsentFromEverySink) {
  test::FakeClock clock;
  auto console = std::make_shared<test::RecordingSink>(SinkKind::Console);
  auto file = std::make_shared<test::RecordingSink>(SinkKind::File);

  AsyncLogger logger(testOptions(clock), {console, file});
  ASSERT_TRUE(logger.start());

  std::vector<Field> fields;
  fields.push_back(classified("token", kSecret, DataClass::Secret));
  fields.push_back(classified("gps", "10.762622,106.660172", DataClass::VehicleSensitive));
  logger.log(Severity::Error, "ota", "download_failed", "could not fetch update",
             std::move(fields));
  logger.flush();

  EXPECT_FALSE(console->containsSubstring(kSecret));
  EXPECT_FALSE(file->containsSubstring(kSecret));
  EXPECT_FALSE(console->containsSubstring("106.660172"));
  EXPECT_TRUE(console->containsSubstring(internal::kRedacted));
  EXPECT_EQ(console->lineCount(), 1u);
  EXPECT_EQ(file->lineCount(), 1u);

  logger.stop();
}

// A public field must still be readable, or redaction would have made logging
// useless rather than safe.
TEST(Redaction, PublicFieldsPassThrough) {
  test::FakeClock clock;
  auto console = std::make_shared<test::RecordingSink>(SinkKind::Console);
  AsyncLogger logger(testOptions(clock), {console});
  ASSERT_TRUE(logger.start());

  std::vector<Field> fields;
  fields.push_back(f("generation", 3));
  fields.push_back(f("reason", std::string("InvalidValue")));
  logger.log(Severity::Warn, "config", "reload_rejected", "", std::move(fields));
  logger.flush();

  EXPECT_TRUE(console->containsSubstring("\"generation\":3"));
  EXPECT_TRUE(console->containsSubstring("\"reason\":\"InvalidValue\""));
  logger.stop();
}

// The message itself is never classified, so a caller that pastes a secret into
// the message text defeats the policy. That is a documented caller
// responsibility (contracts/logging_api.md section 6), and this test pins the
// behaviour so nobody later mistakes it for a bug in redaction.
TEST(Redaction, MessageTextIsNotScanned) {
  LogRecord record;
  record.timestampUtc = test::FakeClock::epoch();
  record.component = "ota";
  record.eventName = "misuse";
  record.message = kSecret;

  const std::string line = internal::formatRecord(record, 16 * 1024).line;
  EXPECT_NE(line.find(kSecret), std::string::npos);
}

}  // namespace
}  // namespace tcu::logging
