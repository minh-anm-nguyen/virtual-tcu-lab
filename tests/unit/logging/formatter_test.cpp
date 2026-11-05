// The output contract of contracts/logging_api.md section 3: one record is one
// line of valid JSON, with a fixed key order, exact escaping, typed values and
// a hard size cap. Timestamps come from an injected clock so these are
// byte-exact golden lines rather than pattern matches (research R-17).

#include <gtest/gtest.h>

#include <string>

#include "json_formatter.hpp"
#include "support/fake_clock.hpp"
#include "tcu/logging/log_types.hpp"

namespace tcu::logging {
namespace {

LogRecord baseRecord() {
  LogRecord record;
  record.timestampUtc = test::FakeClock::epoch();
  record.severity = Severity::Info;
  record.component = "config";
  record.eventName = "startup_loaded";
  record.message = "";
  record.correlationId = "";
  record.fields.push_back(f("generation", 1));
  return record;
}

TEST(Formatter, EmitsWellFormedJsonl) {
  const internal::FormatResult result =
      internal::formatRecord(baseRecord(), 16 * 1024);

  EXPECT_FALSE(result.failed);
  EXPECT_FALSE(result.truncated);
  EXPECT_EQ(result.line,
            "{\"timestamp_utc\":\"2026-10-30T01:00:00.000Z\",\"severity\":\"INFO\","
            "\"component\":\"config\",\"event\":\"startup_loaded\","
            "\"correlation_id\":\"\",\"message\":\"\",\"fields\":{\"generation\":1}}");
}

TEST(Formatter, KeyOrderIsFixed) {
  LogRecord record = baseRecord();
  record.message = "hello";
  record.correlationId = "abc123";
  const std::string line = internal::formatRecord(record, 16 * 1024).line;

  const std::size_t ts = line.find("\"timestamp_utc\"");
  const std::size_t sev = line.find("\"severity\"");
  const std::size_t comp = line.find("\"component\"");
  const std::size_t ev = line.find("\"event\"");
  const std::size_t cid = line.find("\"correlation_id\"");
  const std::size_t msg = line.find("\"message\"");
  const std::size_t fld = line.find("\"fields\"");

  ASSERT_NE(fld, std::string::npos);
  EXPECT_LT(ts, sev);
  EXPECT_LT(sev, comp);
  EXPECT_LT(comp, ev);
  EXPECT_LT(ev, cid);
  EXPECT_LT(cid, msg);
  EXPECT_LT(msg, fld);
}

// Invariant L3: no raw control character may survive, so one record is always
// exactly one line - which is what makes the file parseable with jq.
TEST(Formatter, EscapesControlCharacters) {
  LogRecord record = baseRecord();
  record.fields.clear();
  // The \x01 is split from the next literal on purpose: a hex escape is greedy,
  // so "bell\x01end" would parse as the single character 0x1e.
  record.message = std::string("quote\" back\\ nl\n tab\t cr\r bell\x01" "end");

  const std::string line = internal::formatRecord(record, 16 * 1024).line;

  EXPECT_EQ(line.find('\n'), std::string::npos) << "a raw newline would split the record";
  EXPECT_EQ(line.find('\t'), std::string::npos);
  EXPECT_EQ(line.find('\r'), std::string::npos);
  EXPECT_EQ(line.find('\x01'), std::string::npos);
  EXPECT_NE(line.find("quote\\\""), std::string::npos);
  EXPECT_NE(line.find("back\\\\"), std::string::npos);
  EXPECT_NE(line.find("nl\\n"), std::string::npos);
  EXPECT_NE(line.find("tab\\t"), std::string::npos);
  EXPECT_NE(line.find("cr\\r"), std::string::npos);
  EXPECT_NE(line.find("\\u0001"), std::string::npos);
}

// Invariant L5: the variant type decides the JSON type, not the caller.
TEST(Formatter, FieldTypesRoundTrip) {
  LogRecord record = baseRecord();
  record.fields.clear();
  record.fields.push_back(f("count", std::int64_t{42}));
  record.fields.push_back(f("ratio", 1.5));
  record.fields.push_back(f("ok", true));
  record.fields.push_back(f("name", std::string("telemetry")));

  const std::string line = internal::formatRecord(record, 16 * 1024).line;

  EXPECT_NE(line.find("\"count\":42"), std::string::npos);
  EXPECT_NE(line.find("\"ratio\":1.5"), std::string::npos);
  EXPECT_NE(line.find("\"ok\":true"), std::string::npos);
  EXPECT_NE(line.find("\"name\":\"telemetry\""), std::string::npos);
}

TEST(Formatter, UnicodeIsPreserved) {
  LogRecord record = baseRecord();
  record.fields.clear();
  record.message = "nhiet do dong co: 90 do C";
  const std::string line = internal::formatRecord(record, 16 * 1024).line;
  EXPECT_NE(line.find("nhiet do dong co"), std::string::npos);
}

TEST(Formatter, EmptyFieldsStillEmitsAnObject) {
  LogRecord record = baseRecord();
  record.fields.clear();
  const std::string line = internal::formatRecord(record, 16 * 1024).line;
  EXPECT_NE(line.find("\"fields\":{}"), std::string::npos);
}

TEST(Formatter, SeverityNamesAreUpperCase) {
  LogRecord record = baseRecord();
  record.severity = Severity::Warn;
  EXPECT_NE(internal::formatRecord(record, 16 * 1024).line.find("\"severity\":\"WARN\""),
            std::string::npos);
}

}  // namespace
}  // namespace tcu::logging
