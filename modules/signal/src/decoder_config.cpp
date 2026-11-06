// decoder.json: one schema table, validated before anything starts.
//
// nlohmann/json is used directly here rather than behind a json_document
// wrapper like projects 04 and 05 have. Those wrappers exist to keep the
// dependency out of a library's PUBLIC headers; here the dependency already
// appears in exactly one translation unit and in no header at all, so a third
// copy of the wrapper would add a file without hiding anything more
// (Constitution Principle II - do not duplicate what does not need to exist).

#include "tcu/signal/decoder_config.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace tcu::signal {
namespace {

using Json = nlohmann::json;

/// Hard cap on the sidecar itself, checked before the parse. decoder.json is a
/// handful of keys; anything larger is a mistake or an attack.
constexpr std::size_t kMaxConfigBytes = 256u * 1024u;

enum class FieldType { Int, Bool, String, Object };

struct FieldSpec {
  const char* pointer;
  FieldType type;
  std::int64_t min;
  std::int64_t max;
};

/// contracts/decoder_config.md section 1, in file order. Every key is
/// mandatory: there is no defaults layer, so a typo in a key name fails the
/// load instead of silently restoring a default (project 04's research R-16).
constexpr FieldSpec kFieldSpecs[] = {
    {"/schema_version", FieldType::Int, 1, 1},
    {"/dbc_path", FieldType::String, 1, 4096},
    {"/strict", FieldType::Bool, 0, 0},
    {"/max_dbc_bytes", FieldType::Int, 1024, 16777216},
    {"/frame_mode", FieldType::String, 1, 64},
    {"/payload_length_policy", FieldType::String, 1, 64},
    {"/rx_queue_capacity", FieldType::Int, 16, 65536},
    {"/decoder_workers", FieldType::Int, 1, 1},
    {"/reload/enabled", FieldType::Bool, 0, 0},
    {"/reload/debounce_ms", FieldType::Int, 0, 60000},
    {"/reload/max_attempts_per_min", FieldType::Int, 1, 600},
    {"/signal_freshness/default_timeout_ms", FieldType::Int, 1, 3600000},
    {"/signal_freshness/scan_period_ms", FieldType::Int, 1, 60000},
    {"/signal_freshness/overrides", FieldType::Object, 0, 0},
    {"/invalid_raw_codes", FieldType::Object, 0, 0},
    {"/range_enforcement", FieldType::Object, 0, 0},
    {"/event_publish/mode", FieldType::String, 1, 16},
    {"/event_publish/on_full", FieldType::String, 1, 32},
    {"/logging/rate_limit_per_reason_per_min", FieldType::Int, 0, 100000},
};

void fail(std::vector<ConfigError>& errors, const char* pointer, ConfigErrorKind kind,
          std::string allowed = {}) {
  errors.push_back(ConfigError{pointer, kind, std::move(allowed)});
}

const Json* resolve(const Json& root, const std::string& pointer) {
  const Json* node = &root;
  std::size_t start = 1;
  while (start <= pointer.size()) {
    const std::size_t slash = pointer.find('/', start);
    const std::string token =
        pointer.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!node->is_object()) return nullptr;
    const auto it = node->find(token);
    if (it == node->end()) return nullptr;
    node = &(*it);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return node;
}

/// Every pointer the schema allows, plus the object prefixes on the way there,
/// so an unknown key at any depth is rejected (invariant V-14).
std::set<std::string> allowedPointers() {
  std::set<std::string> allowed;
  for (const auto& spec : kFieldSpecs) {
    const std::string pointer = spec.pointer;
    allowed.insert(pointer);
    std::size_t slash = pointer.rfind('/');
    while (slash != std::string::npos && slash > 0) {
      allowed.insert(pointer.substr(0, slash));
      slash = pointer.substr(0, slash).rfind('/');
    }
  }
  return allowed;
}

void collectUnknown(const Json& node, const std::string& prefix,
                    const std::set<std::string>& allowed,
                    const std::set<std::string>& freeFormRoots,
                    std::vector<ConfigError>& errors) {
  if (!node.is_object()) return;
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string pointer = prefix + "/" + it.key();
    if (allowed.count(pointer) == 0) {
      fail(errors, pointer.c_str(), ConfigErrorKind::UnknownKey);
      continue;
    }
    // The three signal-name maps hold caller-chosen keys, so their contents are
    // not walked against the schema.
    if (freeFormRoots.count(pointer) != 0) continue;
    collectUnknown(*it, pointer, allowed, freeFormRoots, errors);
  }
}

}  // namespace

const char* toString(ConfigErrorKind kind) noexcept {
  switch (kind) {
    case ConfigErrorKind::MissingKey:       return "MissingKey";
    case ConfigErrorKind::UnknownKey:       return "UnknownKey";
    case ConfigErrorKind::WrongType:        return "WrongType";
    case ConfigErrorKind::OutOfRange:       return "OutOfRange";
    case ConfigErrorKind::UnsupportedValue: return "UnsupportedValue";
    case ConfigErrorKind::NotAnObject:      return "NotAnObject";
    case ConfigErrorKind::ParseError:       return "ParseError";
    case ConfigErrorKind::TooLarge:         return "TooLarge";
  }
  return "ParseError";
}

dbc::ParseLimits DecoderConfig::parseLimits() const {
  dbc::ParseLimits limits;
  limits.dbcMaxBytes = maxDbcBytes;
  return limits;
}

ConfigLoadResult parseDecoderConfig(const std::string& text, const std::string& label) {
  (void)label;
  ConfigLoadResult result;

  if (text.size() > kMaxConfigBytes) {
    fail(result.errors, "", ConfigErrorKind::TooLarge, "at most 262144 bytes");
    return result;
  }

  Json root;
  try {
    // Strict: comments rejected, exceptions on, trailing data rejected - the
    // same settings project 04 chose in its research R-01.
    root = Json::parse(text, nullptr, true, false);
  } catch (const std::exception&) {
    fail(result.errors, "", ConfigErrorKind::ParseError);
    return result;
  }
  if (!root.is_object()) {
    fail(result.errors, "", ConfigErrorKind::NotAnObject);
    return result;
  }

  const std::set<std::string> freeForm = {"/signal_freshness/overrides", "/invalid_raw_codes",
                                          "/range_enforcement"};
  collectUnknown(root, "", allowedPointers(), freeForm, result.errors);

  for (const auto& spec : kFieldSpecs) {
    const Json* node = resolve(root, spec.pointer);
    if (node == nullptr) {
      fail(result.errors, spec.pointer, ConfigErrorKind::MissingKey);
      continue;
    }
    switch (spec.type) {
      case FieldType::Int: {
        if (!node->is_number_integer()) {
          fail(result.errors, spec.pointer, ConfigErrorKind::WrongType, "an integer");
          break;
        }
        const std::int64_t value = node->get<std::int64_t>();
        if (value < spec.min || value > spec.max) {
          fail(result.errors, spec.pointer, ConfigErrorKind::OutOfRange,
               std::to_string(spec.min) + ".." + std::to_string(spec.max));
        }
        break;
      }
      case FieldType::Bool:
        if (!node->is_boolean()) {
          fail(result.errors, spec.pointer, ConfigErrorKind::WrongType, "true or false");
        }
        break;
      case FieldType::String: {
        if (!node->is_string()) {
          fail(result.errors, spec.pointer, ConfigErrorKind::WrongType, "a string");
          break;
        }
        const auto length = static_cast<std::int64_t>(node->get<std::string>().size());
        if (length < spec.min || length > spec.max) {
          fail(result.errors, spec.pointer, ConfigErrorKind::OutOfRange,
               std::to_string(spec.min) + ".." + std::to_string(spec.max) + " characters");
        }
        break;
      }
      case FieldType::Object:
        if (!node->is_object()) {
          fail(result.errors, spec.pointer, ConfigErrorKind::WrongType, "an object");
        }
        break;
    }
  }
  if (!result.errors.empty()) return result;

  DecoderConfig config;
  config.schemaVersion = root["schema_version"].get<std::uint32_t>();
  config.dbcPath = root["dbc_path"].get<std::string>();
  config.strict = root["strict"].get<bool>();
  config.maxDbcBytes = root["max_dbc_bytes"].get<std::size_t>();
  config.frameMode = root["frame_mode"].get<std::string>();
  config.payloadLengthPolicy = root["payload_length_policy"].get<std::string>();
  config.rxQueueCapacity = root["rx_queue_capacity"].get<std::size_t>();
  config.decoderWorkers = root["decoder_workers"].get<std::uint32_t>();

  // v1 accepts exactly one value for each of these. The keys stay in the
  // schema so the switch arrives later as a new enum value plus tests, not as
  // a schema change that invalidates every existing file (research R-01, R-02).
  if (config.frameMode != "classical_can") {
    fail(result.errors, "/frame_mode", ConfigErrorKind::UnsupportedValue, "classical_can");
  }
  if (config.payloadLengthPolicy != "exact") {
    fail(result.errors, "/payload_length_policy", ConfigErrorKind::UnsupportedValue, "exact");
  }
  if (!config.strict) {
    fail(result.errors, "/strict", ConfigErrorKind::UnsupportedValue, "true");
  }

  config.reload.enabled = root["reload"]["enabled"].get<bool>();
  config.reload.debounce =
      std::chrono::milliseconds{root["reload"]["debounce_ms"].get<std::int64_t>()};
  config.reload.maxAttemptsPerMinute = root["reload"]["max_attempts_per_min"].get<std::uint32_t>();

  const Json& fresh = root["signal_freshness"];
  config.freshness.defaultTimeout =
      std::chrono::milliseconds{fresh["default_timeout_ms"].get<std::int64_t>()};
  config.scanPeriod = std::chrono::milliseconds{fresh["scan_period_ms"].get<std::int64_t>()};
  for (auto it = fresh["overrides"].begin(); it != fresh["overrides"].end(); ++it) {
    const std::string pointer = "/signal_freshness/overrides/" + it.key();
    if (!it->is_number_integer()) {
      fail(result.errors, pointer.c_str(), ConfigErrorKind::WrongType, "an integer");
      continue;
    }
    const std::int64_t value = it->get<std::int64_t>();
    if (value < 1 || value > 3600000) {
      fail(result.errors, pointer.c_str(), ConfigErrorKind::OutOfRange, "1..3600000");
      continue;
    }
    config.freshness.overrides[it.key()] = std::chrono::milliseconds{value};
  }

  for (auto it = root["invalid_raw_codes"].begin(); it != root["invalid_raw_codes"].end(); ++it) {
    const std::string pointer = "/invalid_raw_codes/" + it.key();
    if (!it->is_array()) {
      fail(result.errors, pointer.c_str(), ConfigErrorKind::WrongType, "an array of integers");
      continue;
    }
    std::vector<std::uint64_t> codes;
    bool bad = false;
    for (const auto& entry : *it) {
      if (!entry.is_number_unsigned()) {
        fail(result.errors, pointer.c_str(), ConfigErrorKind::WrongType,
             "unsigned raw values only");
        bad = true;
        break;
      }
      codes.push_back(entry.get<std::uint64_t>());
    }
    if (!bad) config.invalidRawCodes[it.key()] = std::move(codes);
  }

  for (auto it = root["range_enforcement"].begin(); it != root["range_enforcement"].end(); ++it) {
    const std::string pointer = "/range_enforcement/" + it.key();
    if (!it->is_boolean()) {
      fail(result.errors, pointer.c_str(), ConfigErrorKind::WrongType, "true or false");
      continue;
    }
    config.rangeEnforcement[it.key()] = it->get<bool>();
  }

  const std::string mode = root["event_publish"]["mode"].get<std::string>();
  if (mode == "sync") {
    config.publishMode = EventPublishMode::Sync;
  } else if (mode == "async") {
    config.publishMode = EventPublishMode::Async;
  } else {
    fail(result.errors, "/event_publish/mode", ConfigErrorKind::UnsupportedValue, "sync or async");
  }

  const std::string onFull = root["event_publish"]["on_full"].get<std::string>();
  if (onFull == "drop_and_count") {
    config.onPublishFull = QueueOverflowPolicy::DropAndCount;
  } else if (onFull == "block_bounded") {
    config.onPublishFull = QueueOverflowPolicy::BlockBounded;
  } else {
    fail(result.errors, "/event_publish/on_full", ConfigErrorKind::UnsupportedValue,
         "drop_and_count or block_bounded");
  }

  config.logRateLimitPerReasonPerMinute =
      root["logging"]["rate_limit_per_reason_per_min"].get<std::uint32_t>();

  if (!result.errors.empty()) return result;
  result.ok = true;
  result.config = std::move(config);
  return result;
}

ConfigLoadResult loadDecoderConfig(const std::string& path) {
  ConfigLoadResult result;
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
    fail(result.errors, "", ConfigErrorKind::ParseError, "a readable regular file");
    return result;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fail(result.errors, "", ConfigErrorKind::ParseError, "a readable regular file");
    return result;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parseDecoderConfig(buffer.str(), path);
}

std::vector<std::string> unmatchedPolicyKeys(const DecoderConfig& config,
                                             const dbc::CompiledDbc& db) {
  std::set<std::string> known;
  for (const auto& entry : db.messages()) {
    for (const auto& meta : entry.second.signals) {
      known.insert(entry.second.name + "." + meta.name);
    }
  }
  std::vector<std::string> unmatched;
  const auto check = [&](const std::string& key) {
    if (known.count(key) == 0) unmatched.push_back(key);
  };
  // A key naming no signal would silently disable a check, so it is surfaced
  // rather than ignored (invariant V-16).
  for (const auto& entry : config.invalidRawCodes) check(entry.first);
  for (const auto& entry : config.rangeEnforcement) check(entry.first);
  for (const auto& entry : config.freshness.overrides) check(entry.first);
  return unmatched;
}

}  // namespace tcu::signal
