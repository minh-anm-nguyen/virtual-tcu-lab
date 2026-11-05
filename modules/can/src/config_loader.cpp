#include "tcu/can/can_config.hpp"

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "can_config_schema.hpp"
#include "json_document.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace tcu::can {
namespace {

LoadResult fail(LoadError error, std::string pointer, const char* why) {
  return LoadResult{error, std::move(pointer), why};
}

/// Only vcan0, vcan1, ... are accepted while mode is lab_only. This check is
/// the single thing standing between a configuration typo and a physical bus,
/// so it is a refusal rather than a warning (NFR-01, ADR-07, research R-16).
bool isLabInterface(std::string_view name) {
  if (name.size() < 4 || name.substr(0, 4) != "vcan") {
    return false;
  }
  for (const char c : name.substr(4)) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

/// Every pointer the schema permits: the 13 fixed rows, the seed, and two keys
/// per catalog signal. Built rather than listed, so adding a signal to the
/// catalog cannot leave the schema behind.
std::set<std::string> expectedPointers() {
  std::set<std::string> expected;
  for (const auto& field : kConfigFields) {
    expected.insert(std::string(field.pointer));
  }
  expected.insert(std::string(kSeedPointer));
  for (const auto& signal : kSignals) {
    const std::string base = "/signals/" + std::string(signal.name);
    expected.insert(base + "/can_id");
    expected.insert(base + "/timeout_ms");
  }
  return expected;
}

LoadResult checkNoUnknownOrMissingKeys(const JsonDocument& doc) {
  const std::set<std::string> expected = expectedPointers();

  std::set<std::string> present;
  for (const auto& pointer : doc.leafPointers()) {
    present.insert(pointer);
  }

  // Unknown keys first: a file with both an unknown and a missing key is more
  // often a typo than an omission, and naming the typo is the more useful
  // message.
  for (const auto& pointer : present) {
    if (expected.count(pointer) == 0) {
      return fail(LoadError::SchemaError, pointer, reason::kUnknownKey);
    }
  }
  for (const auto& pointer : expected) {
    if (present.count(pointer) == 0) {
      return fail(LoadError::SchemaError, pointer, reason::kMissingKey);
    }
  }
  return {};
}

LoadResult readInt(const JsonDocument& doc, const std::string& pointer, std::int64_t min,
                   std::int64_t max, std::int64_t& out) {
  const auto value = doc.intAt(pointer);
  if (!value.has_value()) {
    return fail(LoadError::SchemaError, pointer, reason::kWrongType);
  }
  if (*value < min || *value > max) {
    return fail(LoadError::InvalidValue, pointer, reason::kOutOfRange);
  }
  out = *value;
  return {};
}

const FieldSpec& fieldFor(std::string_view pointer) {
  for (const auto& field : kConfigFields) {
    if (field.pointer == pointer) {
      return field;
    }
  }
  // Unreachable: every caller passes a literal from kConfigFields, and
  // ConfigLoader.SchemaTableCoversEveryReadKey asserts that stays true.
  return kConfigFields[0];
}

/// Reads an Int row using the bounds the schema table already states, so a
/// bound is never written twice.
LoadResult readTableInt(const JsonDocument& doc, std::string_view pointer, std::int64_t& out) {
  const FieldSpec& field = fieldFor(pointer);
  return readInt(doc, std::string(pointer), field.min, field.max, out);
}

LoadResult fromParseError(JsonParseError error) {
  switch (error) {
    case JsonParseError::None:
      return {};
    case JsonParseError::FileNotFound:
      return fail(LoadError::FileNotFound, "", reason::kFileNotFound);
    case JsonParseError::ReadError:
      return fail(LoadError::ReadError, "", reason::kReadError);
    case JsonParseError::TooLarge:
      return fail(LoadError::TooLarge, "", reason::kTooLarge);
    case JsonParseError::ParseError:
      return fail(LoadError::ParseError, "", reason::kParseError);
  }
  return fail(LoadError::ParseError, "", reason::kParseError);
}

}  // namespace

LoadResult parseCanLabConfig(const std::string& text, CanLabConfig& out) {
  JsonDocument doc;
  if (const auto parsed = fromParseError(doc.parseString(text)); !parsed) {
    return parsed;
  }

  // The version gate comes before the key audit: a future file is expected to
  // carry keys this build does not know, and reporting "unknown_key" for one
  // would send the reader hunting a typo that is not there.
  const auto version = doc.intAt("/schema_version");
  if (!version.has_value()) {
    return fail(LoadError::SchemaError, "/schema_version", reason::kWrongType);
  }
  if (*version != kSupportedSchemaVersion) {
    return fail(LoadError::UnsupportedVersion, "/schema_version", reason::kUnsupportedVersion);
  }

  if (const auto keys = checkNoUnknownOrMissingKeys(doc); !keys) {
    return keys;
  }

  CanLabConfig cfg;
  cfg.schema_version = *version;

  // --- mode and format: closed enums ---
  const auto mode = doc.stringAt("/mode");
  if (!mode.has_value()) {
    return fail(LoadError::SchemaError, "/mode", reason::kWrongType);
  }
  if (*mode != kModeLabOnly) {
    return fail(LoadError::InvalidValue, "/mode", reason::kOutOfRange);
  }
  cfg.mode = LabMode::LabOnly;

  const auto format = doc.stringAt("/can/format");
  if (!format.has_value()) {
    return fail(LoadError::SchemaError, "/can/format", reason::kWrongType);
  }
  if (*format != kFormatClassicStandard) {
    return fail(LoadError::InvalidValue, "/can/format", reason::kOutOfRange);
  }

  // --- interface ---
  const auto iface = doc.stringAt("/can/interface");
  if (!iface.has_value()) {
    return fail(LoadError::SchemaError, "/can/interface", reason::kWrongType);
  }
  const FieldSpec& iface_spec = fieldFor("/can/interface");
  if (iface->size() < static_cast<std::size_t>(iface_spec.min) ||
      iface->size() > static_cast<std::size_t>(iface_spec.max)) {
    // The upper bound is IFNAMSIZ - 1; a longer name cannot be bound at all.
    return fail(LoadError::InvalidValue, "/can/interface", reason::kOutOfRange);
  }
  if (cfg.mode == LabMode::LabOnly && !isLabInterface(*iface)) {
    return fail(LoadError::InvalidValue, "/can/interface", reason::kInterfaceNotLab);
  }
  cfg.interface_name = *iface;

  // --- integers ---
  std::int64_t capacity = 0;
  std::int64_t health_ms = 0;
  std::int64_t drain_ms = 0;
  std::int64_t reconnect_initial = 0;
  std::int64_t reconnect_max = 0;
  std::int64_t jitter = 0;
  std::int64_t speed_period = 0;
  std::int64_t heartbeat_period = 0;
  std::int64_t seed = 0;

  if (const auto r = readTableInt(doc, "/can/rx_queue_capacity", capacity); !r) return r;
  if (const auto r = readTableInt(doc, "/can/health_check_ms", health_ms); !r) return r;
  if (const auto r = readTableInt(doc, "/can/drain_deadline_ms", drain_ms); !r) return r;
  if (const auto r = readTableInt(doc, "/can/reconnect/initial_ms", reconnect_initial); !r)
    return r;
  if (const auto r = readTableInt(doc, "/can/reconnect/max_ms", reconnect_max); !r) return r;
  if (const auto r = readTableInt(doc, "/can/reconnect/jitter_percent", jitter); !r) return r;
  if (const auto r = readTableInt(doc, "/simulator/speed_period_ms", speed_period); !r) return r;
  if (const auto r = readTableInt(doc, "/simulator/heartbeat_period_ms", heartbeat_period); !r)
    return r;
  if (const auto r = readInt(doc, std::string(kSeedPointer), kSeedMin, kSeedMax, seed); !r)
    return r;

  // Cross-field: a backoff whose floor is above its ceiling has no valid
  // sequence at all, yet neither bound alone is out of range.
  if (reconnect_initial > reconnect_max) {
    return fail(LoadError::InvalidValue, "/can/reconnect/initial_ms", reason::kCrossField);
  }

  cfg.rx_queue_capacity = static_cast<std::size_t>(capacity);
  cfg.health_check_period = std::chrono::milliseconds{health_ms};
  cfg.drain_deadline = std::chrono::milliseconds{drain_ms};
  cfg.reconnect_initial = std::chrono::milliseconds{reconnect_initial};
  cfg.reconnect_max = std::chrono::milliseconds{reconnect_max};
  cfg.reconnect_jitter_pct = static_cast<int>(jitter);
  cfg.simulator.speed_period = std::chrono::milliseconds{speed_period};
  cfg.simulator.heartbeat_period = std::chrono::milliseconds{heartbeat_period};
  cfg.simulator.fixed_seed = static_cast<std::uint32_t>(seed);

  // --- allowed receive identifiers ---
  const auto ids = doc.intArrayAt("/can/allowed_rx_ids");
  if (!ids.has_value()) {
    return fail(LoadError::SchemaError, "/can/allowed_rx_ids", reason::kWrongType);
  }
  if (ids->size() < kMinAllowedRxIds || ids->size() > kMaxAllowedRxIds) {
    return fail(LoadError::InvalidValue, "/can/allowed_rx_ids", reason::kOutOfRange);
  }
  const FieldSpec& ids_spec = fieldFor("/can/allowed_rx_ids");
  for (const auto id : *ids) {
    if (id < ids_spec.min || id > ids_spec.max) {
      return fail(LoadError::InvalidValue, "/can/allowed_rx_ids", reason::kOutOfRange);
    }
    // A filter for a frame no decoder understands is a configuration mistake,
    // not a feature: the kernel would admit the frame and the application would
    // then count it as an unknown identifier forever.
    if (!isKnownFrame(static_cast<std::uint32_t>(id))) {
      return fail(LoadError::InvalidValue, "/can/allowed_rx_ids", reason::kNotInCatalog);
    }
    cfg.allowed_rx_ids.push_back(static_cast<std::uint32_t>(id));
  }

  // --- per-signal rows ---
  // The key audit already proved the set of signal names matches the catalog
  // exactly, so this loop reads rather than discovers.
  for (const auto& signal : kSignals) {
    const std::string base = "/signals/" + std::string(signal.name);

    std::int64_t can_id = 0;
    std::int64_t timeout_ms = 0;
    if (const auto r = readInt(doc, base + "/can_id", 0, kStandardIdMask, can_id); !r) return r;
    if (const auto r =
            readInt(doc, base + "/timeout_ms", kSignalTimeoutMin, kSignalTimeoutMax, timeout_ms);
        !r)
      return r;

    // The catalog decides where a signal lives. A config that disagreed would
    // make the decoder and the freshness monitor read different frames.
    if (static_cast<std::uint32_t>(can_id) != signal.source_id) {
      return fail(LoadError::InvalidValue, base + "/can_id", reason::kNotInCatalog);
    }

    cfg.signals.emplace(std::string(signal.name),
                        SignalConfig{static_cast<std::uint32_t>(can_id),
                                     std::chrono::milliseconds{timeout_ms}});
  }

  out = std::move(cfg);
  return {};
}

LoadResult loadCanLabConfig(const std::string& path, CanLabConfig& out) {
  // Read once, with the size cap applied before anything is held in memory, and
  // then hand the text to the single validation path. Parsing the file here and
  // the text there would be two code paths for one job.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(LoadError::FileNotFound, "", reason::kFileNotFound);
  }

  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) {
    return fail(LoadError::ReadError, "", reason::kReadError);
  }
  if (static_cast<std::size_t>(size) > kMaxConfigBytes) {
    return fail(LoadError::TooLarge, "", reason::kTooLarge);
  }
  in.seekg(0, std::ios::beg);

  // The extra parentheses around the first argument are not decoration: without
  // them this is a function declaration, not a string (the most vexing parse).
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in && !in.eof()) {
    return fail(LoadError::ReadError, "", reason::kReadError);
  }

  return parseCanLabConfig(text, out);
}

}  // namespace tcu::can
