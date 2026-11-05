#include "json_document.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <utility>
#include <vector>

namespace tcu::config::internal {
namespace {

using Json = nlohmann::json;

// Thrown out of the parser callback to stop the parse. Returning false from the
// callback only discards the element, which would silently accept a file with a
// duplicate key - the opposite of what section 9 rule 2 asks for.
struct Abort {
  ParseStatus status;
  std::string detail;
};

void collectInto(const Json& node,
                 const std::string& prefix,
                 std::vector<std::string>& out) {
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      collectInto(it.value(), prefix + "/" + it.key(), out);
    }
    return;
  }
  // Arrays are leaves here: no schema row is an array, so an array value is
  // reported as the unknown or wrong-typed leaf it is.
  out.push_back(prefix);
}

}  // namespace

struct JsonDocument::Impl {
  Json root = Json::object();
  bool loaded = false;
};

JsonDocument::JsonDocument() : impl_(std::make_unique<Impl>()) {}
JsonDocument::~JsonDocument() = default;
JsonDocument::JsonDocument(JsonDocument&&) noexcept = default;
JsonDocument& JsonDocument::operator=(JsonDocument&&) noexcept = default;

ParseOutcome JsonDocument::parse(std::string_view text) {
  ParseOutcome outcome;
  impl_->loaded = false;

  // One key set per open object, so a duplicate is detected where it matters -
  // inside the same object - while two different objects may of course share a
  // key name.
  //
  // shortcut: detection costs memory proportional to the largest object in the
  // file. Upgrade when a configuration file grows objects large enough for that
  // to matter (research R-01).
  std::vector<std::set<std::string>> open_objects;
  std::vector<std::string> path;

  const auto callback = [&](int depth, Json::parse_event_t event,
                            Json& parsed) -> bool {
    if (depth > static_cast<int>(kMaxDepth)) {
      throw Abort{ParseStatus::TooDeep, {}};
    }
    switch (event) {
      case Json::parse_event_t::object_start:
        open_objects.emplace_back();
        break;
      case Json::parse_event_t::object_end:
        if (!open_objects.empty()) {
          open_objects.pop_back();
        }
        if (!path.empty()) {
          path.pop_back();
        }
        break;
      case Json::parse_event_t::key: {
        const std::string key = parsed.get<std::string>();
        if (!open_objects.empty() && !open_objects.back().insert(key).second) {
          std::string pointer;
          for (const std::string& segment : path) {
            pointer += "/" + segment;
          }
          pointer += "/" + key;
          throw Abort{ParseStatus::DuplicateKey, pointer};
        }
        path.push_back(key);
        break;
      }
      case Json::parse_event_t::value:
        if (!path.empty()) {
          path.pop_back();
        }
        break;
      default:
        break;
    }
    return true;
  };

  try {
    impl_->root = Json::parse(text, callback, /*allow_exceptions=*/true);
  } catch (const Abort& abort) {
    outcome.status = abort.status;
    outcome.duplicatePointer = abort.detail;
    return outcome;
  } catch (const Json::parse_error& error) {
    outcome.status = ParseStatus::Malformed;
    outcome.byteOffset = error.byte;
    return outcome;
  } catch (...) {
    outcome.status = ParseStatus::Malformed;
    return outcome;
  }

  if (!impl_->root.is_object()) {
    outcome.status = ParseStatus::NotAnObject;
    return outcome;
  }

  impl_->loaded = true;
  return outcome;
}

bool JsonDocument::has(std::string_view pointer) const {
  if (!impl_->loaded) {
    return false;
  }
  // Braces, not parentheses: a parenthesised form here is a function
  // declaration, not a variable (most vexing parse).
  const Json::json_pointer jp{std::string(pointer)};
  return impl_->root.contains(jp);
}

bool JsonDocument::getInt(std::string_view pointer, std::int64_t& out) const {
  if (!has(pointer)) {
    return false;
  }
  const Json& node = impl_->root[Json::json_pointer{std::string(pointer)}];
  // is_number_integer is false for 5.0, which is the point: the schema says
  // int, and a float is a type error rather than something to truncate.
  if (!node.is_number_integer()) {
    return false;
  }
  out = node.get<std::int64_t>();
  return true;
}

bool JsonDocument::getBool(std::string_view pointer, bool& out) const {
  if (!has(pointer)) {
    return false;
  }
  const Json& node = impl_->root[Json::json_pointer{std::string(pointer)}];
  if (!node.is_boolean()) {
    return false;
  }
  out = node.get<bool>();
  return true;
}

bool JsonDocument::getString(std::string_view pointer, std::string& out) const {
  if (!has(pointer)) {
    return false;
  }
  const Json& node = impl_->root[Json::json_pointer{std::string(pointer)}];
  if (!node.is_string()) {
    return false;
  }
  out = node.get<std::string>();
  return true;
}

std::vector<std::string> JsonDocument::collectPointers() const {
  std::vector<std::string> out;
  if (impl_->loaded) {
    collectInto(impl_->root, "", out);
  }
  return out;
}

}  // namespace tcu::config::internal
