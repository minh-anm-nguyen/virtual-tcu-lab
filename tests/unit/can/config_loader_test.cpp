// Configuration validation.
//
// Two rules drive most of these cases. First, every key is mandatory and an
// unknown key is an error, so a typo fails startup rather than being silently
// ignored. Second, a reason string is a log line, so it is chosen from a closed
// template set and never built from the file contents (Constitution Principle
// V, contracts/config_schema.md section 4).

#include "tcu/can/can_config.hpp"

#include <gtest/gtest.h>

#include <string>

#include "can_config_schema.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;

// The shipped sample, as text, so a case can mutate one key and leave the rest
// valid. ConfigLoader.ParsedSampleEqualsTheShippedFile checks this stays in
// step with the real config/can_lab.json.
std::string validConfig() {
  return R"json({
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": 1024,
    "health_check_ms": 50,
    "drain_deadline_ms": 200,
    "reconnect": { "initial_ms": 250, "max_ms": 5000, "jitter_percent": 10 },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 500  },
    "battery_voltage_v": { "can_id": 258, "timeout_ms": 3000 },
    "battery_soc_pct":   { "can_id": 258, "timeout_ms": 3000 },
    "door_state":        { "can_id": 259, "timeout_ms": 3000 },
    "heartbeat":         { "can_id": 272, "timeout_ms": 3000 },
    "ecu_status":        { "can_id": 272, "timeout_ms": 3000 }
  },
  "simulator": { "speed_period_ms": 100, "heartbeat_period_ms": 1000, "fixed_seed": 42 }
})json";
}

/// Replaces the first occurrence of `from` with `to`, so a case can state the
/// single thing it changed instead of restating the whole document.
std::string withEdit(const std::string& from, const std::string& to) {
  std::string text = validConfig();
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos) << "the fixture no longer contains: " << from;
  if (at != std::string::npos) {
    text.replace(at, from.size(), to);
  }
  return text;
}

LoadResult parse(const std::string& text) {
  CanLabConfig cfg;
  return parseCanLabConfig(text, cfg);
}

std::string samplePath() { return std::string(TCU_CAN_CONFIG_DIR) + "/can_lab.json"; }

TEST(ConfigLoader, AcceptsTheSampleDocument) {
  CanLabConfig cfg;
  const auto result = parseCanLabConfig(validConfig(), cfg);
  ASSERT_TRUE(result) << result.pointer << " " << result.publicReason;

  EXPECT_EQ(cfg.interface_name, "vcan0");
  EXPECT_EQ(cfg.rx_queue_capacity, 1024u);
  EXPECT_EQ(cfg.health_check_period.count(), 50);
  EXPECT_EQ(cfg.drain_deadline.count(), 200);
  EXPECT_EQ(cfg.reconnect_initial.count(), 250);
  EXPECT_EQ(cfg.reconnect_max.count(), 5000);
  EXPECT_EQ(cfg.reconnect_jitter_pct, 10);
  EXPECT_EQ(cfg.allowed_rx_ids.size(), 6u);
  EXPECT_EQ(cfg.signals.size(), kSignalCount);
  EXPECT_EQ(cfg.signals.at("speed_kmh").timeout.count(), 500);
  EXPECT_EQ(cfg.signals.at("speed_kmh").can_id, 0x101u);
  EXPECT_EQ(cfg.simulator.fixed_seed, 42u);
}

// The shipped file and the schema table must not drift apart. Reading the real
// file rather than a copy is the point of this case.
TEST(ConfigLoader, SampleFileMatchesTable) {
  CanLabConfig cfg;
  const auto result = loadCanLabConfig(samplePath(), cfg);

  ASSERT_TRUE(result) << samplePath() << ": " << result.pointer << " " << result.publicReason;
  EXPECT_EQ(cfg.signals.size(), kSignalCount);
  for (const auto& signal : kSignals) {
    const auto it = cfg.signals.find(std::string(signal.name));
    ASSERT_NE(it, cfg.signals.end()) << signal.name << " missing from the shipped file";
    EXPECT_EQ(it->second.can_id, signal.source_id) << signal.name;
  }
}

TEST(ConfigLoader, ParsedSampleEqualsTheShippedFile) {
  // If these two disagreed, every other case in this file would be testing a
  // document nobody ships.
  CanLabConfig from_text;
  CanLabConfig from_file;
  ASSERT_TRUE(parseCanLabConfig(validConfig(), from_text));
  ASSERT_TRUE(loadCanLabConfig(samplePath(), from_file));

  EXPECT_EQ(from_text.interface_name, from_file.interface_name);
  EXPECT_EQ(from_text.rx_queue_capacity, from_file.rx_queue_capacity);
  EXPECT_EQ(from_text.health_check_period, from_file.health_check_period);
  EXPECT_EQ(from_text.drain_deadline, from_file.drain_deadline);
  EXPECT_EQ(from_text.reconnect_initial, from_file.reconnect_initial);
  EXPECT_EQ(from_text.reconnect_max, from_file.reconnect_max);
  EXPECT_EQ(from_text.reconnect_jitter_pct, from_file.reconnect_jitter_pct);
  EXPECT_EQ(from_text.allowed_rx_ids, from_file.allowed_rx_ids);
  EXPECT_EQ(from_text.simulator.fixed_seed, from_file.simulator.fixed_seed);
}

TEST(ConfigLoader, RejectsUnknownKey) {
  const auto result = parse(withEdit("\"health_check_ms\": 50", "\"health_check_msec\": 50"));

  EXPECT_EQ(result.error, LoadError::SchemaError);
  EXPECT_EQ(result.publicReason, reason::kUnknownKey);
  EXPECT_EQ(result.pointer, "/can/health_check_msec") << "the typo is named";
}

TEST(ConfigLoader, RejectsMissingKey) {
  const auto result = parse(withEdit("\"drain_deadline_ms\": 200,", ""));

  EXPECT_EQ(result.error, LoadError::SchemaError);
  EXPECT_EQ(result.publicReason, reason::kMissingKey);
  EXPECT_EQ(result.pointer, "/can/drain_deadline_ms");
}

TEST(ConfigLoader, RejectsMissingSignalKey) {
  // Dropping a per-signal key must fail rather than fall back to a catalog
  // default: a half-configured signal table is how a deadline silently changes.
  const auto result =
      parse(withEdit("\"ecu_status\":        { \"can_id\": 272, \"timeout_ms\": 3000 }",
                     "\"ecu_status\":        { \"can_id\": 272 }"));

  EXPECT_EQ(result.error, LoadError::SchemaError);
  EXPECT_EQ(result.publicReason, reason::kMissingKey);
  EXPECT_EQ(result.pointer, "/signals/ecu_status/timeout_ms");
}

TEST(ConfigLoader, RejectsUnknownSignalName) {
  const auto result = parse(withEdit("\"door_state\"", "\"doors\""));

  EXPECT_EQ(result.error, LoadError::SchemaError);
  // Reported as an unknown key, which is what it is: the pointer does not
  // exist in the schema at all.
  EXPECT_EQ(result.publicReason, reason::kUnknownKey);
}

TEST(ConfigLoader, RejectsOutOfRange) {
  EXPECT_EQ(parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": 0")).publicReason,
            reason::kOutOfRange)
      << "NFR-02 forbids a capacity of zero";
  EXPECT_EQ(
      parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": 65537")).publicReason,
      reason::kOutOfRange);
  EXPECT_EQ(parse(withEdit("\"jitter_percent\": 10", "\"jitter_percent\": 51")).publicReason,
            reason::kOutOfRange);
  EXPECT_EQ(parse(withEdit("\"health_check_ms\": 50", "\"health_check_ms\": 0")).publicReason,
            reason::kOutOfRange);
}

TEST(ConfigLoader, AcceptsInclusiveBounds) {
  // Both bounds are inclusive, so neither edge may be rejected.
  EXPECT_TRUE(parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": 1")));
  EXPECT_TRUE(parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": 65536")));
  EXPECT_TRUE(parse(withEdit("\"jitter_percent\": 10", "\"jitter_percent\": 0")));
  EXPECT_TRUE(parse(withEdit("\"jitter_percent\": 10", "\"jitter_percent\": 50")));
  EXPECT_TRUE(parse(withEdit("\"drain_deadline_ms\": 200", "\"drain_deadline_ms\": 0")))
      << "a zero drain deadline is legal: it means discard immediately";
}

TEST(ConfigLoader, RejectsWrongType) {
  EXPECT_EQ(parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": \"1024\""))
                .publicReason,
            reason::kWrongType)
      << "a numeric string is not an integer";
  EXPECT_EQ(
      parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": 1024.5")).publicReason,
      reason::kWrongType);
  EXPECT_EQ(parse(withEdit("\"interface\": \"vcan0\"", "\"interface\": 0")).publicReason,
            reason::kWrongType);
}

// The cross-field rule: neither bound is out of range on its own.
TEST(ConfigLoader, RejectsReconnectInitialAboveMax) {
  const auto result = parse(
      withEdit("\"initial_ms\": 250, \"max_ms\": 5000", "\"initial_ms\": 5000, \"max_ms\": 250"));

  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.publicReason, reason::kCrossField);
  EXPECT_EQ(result.pointer, "/can/reconnect/initial_ms");
}

TEST(ConfigLoader, AcceptsEqualReconnectBounds) {
  // initial == max means a constant retry interval, which is legal.
  EXPECT_TRUE(parse(
      withEdit("\"initial_ms\": 250, \"max_ms\": 5000", "\"initial_ms\": 250, \"max_ms\": 250")));
}

// NFR-01, ADR-07, research R-16. The most important case in this file.
TEST(ConfigLoader, RejectsNonLabInterface) {
  // "vcan" with no index is NOT here: the documented pattern is vcan[0-9]*,
  // whose * admits zero digits, and spec section 24 is broader still ("only
  // vcan* interfaces accepted"). AcceptsAnyVcanIndex covers it.
  for (const char* name : {"can0", "can1", "eth0", "vcanx", "vcan0x", "wwan0", ""}) {
    const auto result =
        parse(withEdit("\"interface\": \"vcan0\"", std::string("\"interface\": \"") + name + "\""));
    EXPECT_FALSE(result) << "accepted interface: " << name;
    EXPECT_EQ(result.error, LoadError::InvalidValue) << name;
  }
}

TEST(ConfigLoader, RejectsNonLabInterfaceWithItsOwnReason) {
  // Distinguishable from a length problem, so an operator knows it was the
  // safety rule rather than a name that was too long.
  const auto result = parse(withEdit("\"interface\": \"vcan0\"", "\"interface\": \"can0\""));
  EXPECT_EQ(result.publicReason, reason::kInterfaceNotLab);
  EXPECT_EQ(result.pointer, "/can/interface");
}

TEST(ConfigLoader, AcceptsAnyVcanIndex) {
  // "vcan" is accepted for the reason spelled out in RejectsNonLabInterface:
  // the pattern admits zero digits.
  for (const char* name : {"vcan", "vcan0", "vcan1", "vcan9", "vcan42"}) {
    EXPECT_TRUE(
        parse(withEdit("\"interface\": \"vcan0\"", std::string("\"interface\": \"") + name + "\"")))
        << name;
  }
}

TEST(ConfigLoader, RejectsOverlongInterfaceName) {
  // 16 characters: one past IFNAMSIZ - 1, so it could never be bound.
  const auto result =
      parse(withEdit("\"interface\": \"vcan0\"", "\"interface\": \"vcan123456789012\""));
  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.publicReason, reason::kOutOfRange);
}

TEST(ConfigLoader, RejectsUnsupportedVersion) {
  const auto result = parse(withEdit("\"schema_version\": 1", "\"schema_version\": 2"));

  // Distinct from InvalidValue: an operator handed a future file needs to know
  // that specifically, not that some number was out of range.
  EXPECT_EQ(result.error, LoadError::UnsupportedVersion);
  EXPECT_EQ(result.publicReason, reason::kUnsupportedVersion);
}

TEST(ConfigLoader, VersionGateRunsBeforeTheKeyAudit) {
  // A future file carries keys this build does not know. Reporting
  // "unknown_key" would send the reader hunting a typo that is not there.
  std::string text = withEdit("\"schema_version\": 1", "\"schema_version\": 7");
  const auto at = text.find("\"simulator\"");
  ASSERT_NE(at, std::string::npos);
  text.insert(at, "\"future_section\": { \"whatever\": 1 },\n  ");

  EXPECT_EQ(parse(text).error, LoadError::UnsupportedVersion);
}

TEST(ConfigLoader, RejectsUnknownMode) {
  // "unrestricted" does not exist in V1, so there is no runtime path to a
  // physical interface at all (research R-16).
  const auto result = parse(withEdit("\"mode\": \"lab_only\"", "\"mode\": \"unrestricted\""));

  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.pointer, "/mode");
}

TEST(ConfigLoader, RejectsUnknownFormat) {
  // CAN FD is a V2 extension (spec section 10.5, research R-24).
  const auto result =
      parse(withEdit("\"format\": \"classic_standard_11bit\"", "\"format\": \"can_fd\""));
  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.pointer, "/can/format");
}

TEST(ConfigLoader, RejectsSignalIdMismatchingCatalog) {
  // The catalog decides where a signal lives. A config that disagreed would
  // make the decoder and the freshness monitor read different frames.
  const auto result = parse(withEdit("\"speed_kmh\":         { \"can_id\": 257",
                                     "\"speed_kmh\":         { \"can_id\": 258"));

  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.publicReason, reason::kNotInCatalog);
  EXPECT_EQ(result.pointer, "/signals/speed_kmh/can_id");
}

TEST(ConfigLoader, RejectsFilterIdNotInCatalog) {
  // A filter for a frame no decoder understands is a configuration mistake: the
  // kernel admits it and the application counts it as unknown forever.
  const auto result =
      parse(withEdit("[256, 257, 258, 259, 272, 513]", "[256, 257, 258, 259, 272, 1365]"));

  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.publicReason, reason::kNotInCatalog);
}

TEST(ConfigLoader, RejectsFilterIdAboveElevenBits) {
  const auto result =
      parse(withEdit("[256, 257, 258, 259, 272, 513]", "[256, 257, 258, 259, 272, 2048]"));
  EXPECT_EQ(result.error, LoadError::InvalidValue);
}

TEST(ConfigLoader, RejectsEmptyFilterList) {
  const auto result = parse(withEdit("[256, 257, 258, 259, 272, 513]", "[]"));
  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(result.publicReason, reason::kOutOfRange);
}

TEST(ConfigLoader, RejectsMalformedJson) {
  const auto result = parse("{ \"schema_version\": 1,");
  EXPECT_EQ(result.error, LoadError::ParseError);
  EXPECT_EQ(result.publicReason, reason::kParseError);
}

TEST(ConfigLoader, RejectsNonObjectDocument) {
  EXPECT_EQ(parse("[1, 2, 3]").error, LoadError::ParseError);
  EXPECT_EQ(parse("42").error, LoadError::ParseError);
}

TEST(ConfigLoader, ReportsMissingFile) {
  CanLabConfig cfg;
  const auto result = loadCanLabConfig(std::string(TCU_CAN_CONFIG_DIR) + "/not_here.json", cfg);

  EXPECT_EQ(result.error, LoadError::FileNotFound);
  EXPECT_EQ(result.publicReason, reason::kFileNotFound);
}

// Constitution Principle V: an error message is a log line, so it must not
// carry a value from an untrusted file. This case is the guard on that rule.
TEST(ConfigLoader, PublicReasonNeverEchoesValue) {
  const std::string rejected = "99999999";
  const auto result =
      parse(withEdit("\"rx_queue_capacity\": 1024", "\"rx_queue_capacity\": " + rejected));

  ASSERT_FALSE(result);
  EXPECT_EQ(result.publicReason.find(rejected), std::string::npos)
      << "publicReason leaked the rejected value: " << result.publicReason;

  // And it is one of the closed templates, not free text.
  const bool known = result.publicReason == reason::kOutOfRange ||
                     result.publicReason == reason::kWrongType ||
                     result.publicReason == reason::kUnknownKey ||
                     result.publicReason == reason::kMissingKey ||
                     result.publicReason == reason::kCrossField ||
                     result.publicReason == reason::kNotInCatalog ||
                     result.publicReason == reason::kInterfaceNotLab ||
                     result.publicReason == reason::kUnsupportedVersion;
  EXPECT_TRUE(known) << result.publicReason;
}

TEST(ConfigLoader, FailureLeavesTheOutputUntouched) {
  // Spec section 12.1: an invalid configuration means nothing was applied.
  CanLabConfig cfg;
  cfg.interface_name = "untouched";
  const auto result = parseCanLabConfig("{ \"schema_version\": 1, \"bogus\": 1 }", cfg);

  ASSERT_FALSE(result);
  EXPECT_EQ(cfg.interface_name, "untouched") << "a partial configuration was applied";
}

TEST(ConfigLoader, SchemaTableCoversEveryReadKey) {
  // fieldFor() falls back to row 0 for an unknown pointer, which would silently
  // apply the wrong bounds. This case keeps that fallback unreachable.
  for (const char* pointer :
       {"/schema_version", "/mode", "/can/interface", "/can/format", "/can/rx_queue_capacity",
        "/can/health_check_ms", "/can/drain_deadline_ms", "/can/reconnect/initial_ms",
        "/can/reconnect/max_ms", "/can/reconnect/jitter_percent", "/can/allowed_rx_ids",
        "/simulator/speed_period_ms", "/simulator/heartbeat_period_ms"}) {
    bool found = false;
    for (const auto& field : kConfigFields) {
      found = found || field.pointer == pointer;
    }
    EXPECT_TRUE(found) << pointer << " is read but absent from kConfigFields";
  }
  EXPECT_EQ(kConfigFields.size(), 13u);
}

}  // namespace
