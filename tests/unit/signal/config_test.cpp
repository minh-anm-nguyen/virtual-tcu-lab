// T042: the decoder.json schema. One table, no defaults layer, unknown keys
// rejected at any depth, and an error that names the pointer but never the
// value (invariants V-14..V-17).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/signal/decoder_config.hpp"

namespace {

using tcu::signal::ConfigErrorKind;

std::string sampleJson() {
  return R"({
  "schema_version": 1,
  "dbc_path": "./configs/virtual_tcu.dbc",
  "strict": true,
  "max_dbc_bytes": 2097152,
  "frame_mode": "classical_can",
  "payload_length_policy": "exact",
  "rx_queue_capacity": 4096,
  "decoder_workers": 1,
  "reload": {"enabled": true, "debounce_ms": 300, "max_attempts_per_min": 6},
  "signal_freshness": {"default_timeout_ms": 500, "scan_period_ms": 50, "overrides": {}},
  "invalid_raw_codes": {},
  "range_enforcement": {},
  "event_publish": {"mode": "async", "on_full": "drop_and_count"},
  "logging": {"rate_limit_per_reason_per_min": 60}
})";
}

bool hasError(const std::vector<tcu::signal::ConfigError>& errors, const std::string& pointer,
              ConfigErrorKind kind) {
  for (const auto& error : errors) {
    if (error.pointer == pointer && error.kind == kind) return true;
  }
  return false;
}

std::string withReplacement(const std::string& from, const std::string& to) {
  std::string text = sampleJson();
  const std::size_t at = text.find(from);
  if (at == std::string::npos) return text;
  return text.replace(at, from.size(), to);
}

TEST(Config_SampleFileMatchesTable, ShippedFileLoadsAndEveryValueIsInRange) {
  // V-14: one table is only a single source of truth if the file we ship
  // cannot drift away from it.
  const auto result = tcu::signal::loadDecoderConfig("configs/decoder.json");
  ASSERT_TRUE(result.ok) << (result.errors.empty()
                                 ? ""
                                 : result.errors[0].pointer + " " +
                                       tcu::signal::toString(result.errors[0].kind));
  EXPECT_EQ(result.config.schemaVersion, 1u);
  EXPECT_EQ(result.config.rxQueueCapacity, 4096u);
  EXPECT_EQ(result.config.decoderWorkers, 1u);
  EXPECT_EQ(result.config.freshness.defaultTimeout, std::chrono::milliseconds{500});
  EXPECT_EQ(result.config.scanPeriod, std::chrono::milliseconds{50});
  ASSERT_EQ(result.config.invalidRawCodes.count("VEHICLE_STATUS.VehicleSpeed"), 1u);
  EXPECT_EQ(result.config.invalidRawCodes.at("VEHICLE_STATUS.VehicleSpeed")[0], 65535u);
  EXPECT_TRUE(result.config.rangeEnforcement.at("VEHICLE_STATUS.VehicleSpeed"));
  EXPECT_EQ(result.config.freshness.overrides.at("TORQUE_STATUS.Torque"),
            std::chrono::milliseconds{200});
}

TEST(Config_SampleFileMatchesTable, InlineSampleAlsoLoads) {
  EXPECT_TRUE(tcu::signal::parseDecoderConfig(sampleJson(), "inline").ok);
}

TEST(Config_RejectsUnknownKey, AtTopLevel) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"schema_version\": 1,", "\"schema_version\": 1, \"telemetry\": 5,"),
      "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/telemetry", ConfigErrorKind::UnknownKey));
}

TEST(Config_RejectsUnknownKey, Nested) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"enabled\": true,", "\"enabled\": true, \"retries\": 2,"), "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/reload/retries", ConfigErrorKind::UnknownKey));
}

TEST(Config_RejectsOutOfRange, QueueCapacity) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"rx_queue_capacity\": 4096", "\"rx_queue_capacity\": 999999"), "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/rx_queue_capacity", ConfigErrorKind::OutOfRange));
}

TEST(Config_RejectsWrongType, TimeoutAsString) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"default_timeout_ms\": 500", "\"default_timeout_ms\": \"500\""), "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(
      hasError(result.errors, "/signal_freshness/default_timeout_ms", ConfigErrorKind::WrongType));
}

TEST(Config_RejectsMissingKey, NoDefaultsLayer) {
  const auto result =
      tcu::signal::parseDecoderConfig(withReplacement("\"rx_queue_capacity\": 4096,", ""),
                                      "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/rx_queue_capacity", ConfigErrorKind::MissingKey));
}

TEST(Config_RejectsNonClassicalFrameMode, CanFdIsNotSilentlyAccepted) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"frame_mode\": \"classical_can\"", "\"frame_mode\": \"can_fd\""),
      "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/frame_mode", ConfigErrorKind::UnsupportedValue));
}

TEST(Config_RejectsMultipleWorkers, Adr05SingleWorker) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"decoder_workers\": 1", "\"decoder_workers\": 4"), "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/decoder_workers", ConfigErrorKind::OutOfRange));
}

TEST(Config_RejectsNonStrict, LenientModeIsV2) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"strict\": true", "\"strict\": false"), "inline");
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(hasError(result.errors, "/strict", ConfigErrorKind::UnsupportedValue));
}

TEST(Config_ErrorNamesPointerNotValue, NeverEchoesTheOffendingNumber) {
  const auto result = tcu::signal::parseDecoderConfig(
      withReplacement("\"rx_queue_capacity\": 4096", "\"rx_queue_capacity\": 123456789"),
      "inline");
  ASSERT_FALSE(result.ok);
  for (const auto& error : result.errors) {
    EXPECT_EQ(error.pointer.find("123456789"), std::string::npos);
    EXPECT_EQ(error.allowed.find("123456789"), std::string::npos)
        << "the allowed range may be shown, the supplied value may not";
  }
  EXPECT_TRUE(hasError(result.errors, "/rx_queue_capacity", ConfigErrorKind::OutOfRange));
}

TEST(Config_RejectsMalformedJson, NoExceptionEscapes) {
  EXPECT_NO_THROW({
    for (const char* text : {"", "{", "not json", "[]", "{\"a\":}", "{} trailing"}) {
      const auto result = tcu::signal::parseDecoderConfig(text, "inline");
      EXPECT_FALSE(result.ok) << text;
      EXPECT_FALSE(result.errors.empty()) << text;
    }
  });
}

TEST(Config_WarnsOnUnmatchedSignalKey, TypoIsSurfacedNotIgnored) {
  const auto loaded = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n",
      "lab.dbc");
  ASSERT_TRUE(loaded.ok());

  const auto config = tcu::signal::parseDecoderConfig(
      withReplacement("\"invalid_raw_codes\": {}",
                      "\"invalid_raw_codes\": {\"VEHICLE_STATUS.VehicelSpeed\": [65535]}"),
      "inline");
  ASSERT_TRUE(config.ok);

  const auto unmatched = tcu::signal::unmatchedPolicyKeys(config.config, *loaded.db);
  ASSERT_EQ(unmatched.size(), 1u);
  EXPECT_EQ(unmatched[0], "VEHICLE_STATUS.VehicelSpeed");
}

TEST(Config_WarnsOnUnmatchedSignalKey, CorrectKeysProduceNoWarning) {
  const auto loaded = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n",
      "lab.dbc");
  ASSERT_TRUE(loaded.ok());
  const auto config = tcu::signal::parseDecoderConfig(
      withReplacement("\"invalid_raw_codes\": {}",
                      "\"invalid_raw_codes\": {\"VEHICLE_STATUS.VehicleSpeed\": [65535]}"),
      "inline");
  ASSERT_TRUE(config.ok);
  EXPECT_TRUE(tcu::signal::unmatchedPolicyKeys(config.config, *loaded.db).empty());
}

}  // namespace
