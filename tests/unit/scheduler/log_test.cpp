// FR-12 log half and Principle V: the field set is closed and no value a
// callback could carry has a path into a line.
// Contract: contracts/metrics.md section 3.

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

#include "log.hpp"

using tcu::scheduler::detail::format;
using tcu::scheduler::detail::LogEvent;
using tcu::scheduler::detail::LogFields;
using tcu::scheduler::detail::LogLevel;
using tcu::scheduler::detail::LogMode;
using tcu::scheduler::detail::LogReason;

namespace {

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t sep = line.find(' ', start);
    if (sep == std::string::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, sep - start));
    start = sep + 1;
  }
  return out;
}

}  // namespace

// The strongest form of "no secret can leak": there is no owning string field,
// so there is no parameter a payload could travel through.
static_assert(std::is_trivially_copyable<LogFields>::value,
              "LogFields must carry no owning string field");

TEST(Log, FieldOrderAndCount) {
  LogFields f;
  f.level = LogLevel::Info;
  f.event = LogEvent::Cancel;
  f.reason = LogReason::Cancelled;
  f.mode = LogMode::Once;
  f.timer_id = 42;
  f.value = 3000;

  const auto parts = split(format(f));
  ASSERT_EQ(parts.size(), 8u) << format(f);
  EXPECT_EQ(parts[1], "INFO");
  EXPECT_EQ(parts[2], "scheduler");
  EXPECT_EQ(parts[3], "42");
  EXPECT_EQ(parts[4], "once");
  EXPECT_EQ(parts[5], "cancel");
  EXPECT_EQ(parts[6], "cancelled");
  EXPECT_EQ(parts[7], "3000");
  EXPECT_EQ(format(f).find('\n'), std::string::npos) << "format adds no newline";
}

TEST(Log, AbsentOptionalsRenderAsDash) {
  LogFields f;
  f.level = LogLevel::Warn;
  f.event = LogEvent::Backpressure;

  const auto parts = split(format(f));
  ASSERT_EQ(parts.size(), 8u);
  EXPECT_EQ(parts[1], "WARN");
  EXPECT_EQ(parts[3], "-") << "no timer id";
  EXPECT_EQ(parts[4], "-") << "no mode";
  EXPECT_EQ(parts[6], "-") << "no reason";
  EXPECT_EQ(parts[7], "-") << "no value";
}

TEST(Log, TimestampIsIso8601UtcMillis) {
  LogFields f;
  const auto parts = split(format(f));
  ASSERT_FALSE(parts.empty());
  const std::regex iso(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)");
  EXPECT_TRUE(std::regex_match(parts[0], iso)) << parts[0];
}

TEST(Log, EveryLevelRenders) {
  for (const auto& pair : std::vector<std::pair<LogLevel, std::string>>{
           {LogLevel::Debug, "DEBUG"},
           {LogLevel::Info, "INFO"},
           {LogLevel::Warn, "WARN"},
           {LogLevel::Error, "ERROR"}}) {
    LogFields f;
    f.level = pair.first;
    EXPECT_EQ(split(format(f))[1], pair.second);
  }
}

TEST(Log, EventVocabularyIsClosed) {
  // Spec section 15 names exactly these six events; the enum is the vocabulary,
  // so a seventh cannot be logged without changing the type.
  const std::vector<std::pair<LogEvent, std::string>> expected{
      {LogEvent::Start, "start"},         {LogEvent::Schedule, "schedule"},
      {LogEvent::Cancel, "cancel"},       {LogEvent::Backpressure, "backpressure"},
      {LogEvent::Callback, "callback"},   {LogEvent::Stop, "stop"}};
  for (const auto& pair : expected) {
    LogFields f;
    f.event = pair.first;
    EXPECT_EQ(split(format(f))[5], pair.second);
  }
}

TEST(Log, ModeVocabularyIsClosed) {
  const std::vector<std::pair<LogMode, std::string>> expected{
      {LogMode::None, "-"},
      {LogMode::Once, "once"},
      {LogMode::FixedDelay, "fixed_delay"},
      {LogMode::FixedRate, "fixed_rate"}};
  for (const auto& pair : expected) {
    LogFields f;
    f.mode = pair.first;
    EXPECT_EQ(split(format(f))[4], pair.second);
  }
}

TEST(Log, RendersTheSpecSection15Lines) {
  // INFO scheduler.start workers=2
  LogFields start;
  start.level = LogLevel::Info;
  start.event = LogEvent::Start;
  start.value = 2;
  EXPECT_EQ(split(format(start))[7], "2");

  // DEBUG scheduler.schedule id=42 mode=once delay_ms=3000
  LogFields schedule;
  schedule.level = LogLevel::Debug;
  schedule.event = LogEvent::Schedule;
  schedule.mode = LogMode::Once;
  schedule.timer_id = 42;
  schedule.value = 3000;
  const auto s = split(format(schedule));
  EXPECT_EQ(s[3], "42");
  EXPECT_EQ(s[4], "once");
  EXPECT_EQ(s[7], "3000");

  // ERROR scheduler.callback id=99 error=exception action=periodic_disabled
  LogFields failure;
  failure.level = LogLevel::Error;
  failure.event = LogEvent::Callback;
  failure.reason = LogReason::PeriodicDisabled;
  failure.mode = LogMode::FixedRate;
  failure.timer_id = 99;
  const auto f = split(format(failure));
  EXPECT_EQ(f[1], "ERROR");
  EXPECT_EQ(f[6], "periodic_disabled");

  // INFO scheduler.stop mode=discard in_flight=0
  LogFields stop;
  stop.event = LogEvent::Stop;
  stop.reason = LogReason::Discard;
  stop.value = 0;
  const auto t = split(format(stop));
  EXPECT_EQ(t[5], "stop");
  EXPECT_EQ(t[6], "discard");
  EXPECT_EQ(t[7], "0");
}
