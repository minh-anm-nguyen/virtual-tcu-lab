#include "json_document.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace tcu::can {
namespace {

using Json = nlohmann::json;

// Collects every leaf as an RFC 6901 pointer. An array is one leaf: project 05
// has exactly one array key and treating its elements as separate leaves would
// make the unknown-key comparison depend on how many identifiers are
// configured.
void collectLeaves(const Json& node, const std::string& prefix,
                   std::vector<std::string>& out) {
  if (node.is_object()) {
    if (node.empty()) {
      out.push_back(prefix);
      return;
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
      // RFC 6901 escaping: '~' becomes '~0' and '/' becomes '~1'. No project 05
      // key contains either, but a hostile file can, and an unescaped key would
      // silently match the wrong pointer.
      std::string token = it.key();
      std::string escaped;
      escaped.reserve(token.size());
      for (const char c : token) {
        if (c == '~') {
          escaped += "~0";
        } else if (c == '/') {
          escaped += "~1";
        } else {
          escaped += c;
        }
      }
      collectLeaves(it.value(), prefix + "/" + escaped, out);
    }
    return;
  }
  out.push_back(prefix);
}

const Json* at(const Json& root, const std::string& pointer) {
  // json_pointer throws on a syntactically invalid pointer and at() throws when
  // the path is absent. Both are expected inputs here, not bugs, so neither may
  // escape as an exception.
  try {
    const Json::json_pointer ptr(pointer);
    if (!root.contains(ptr)) {
      return nullptr;
    }
    return &root.at(ptr);
  } catch (const Json::exception&) {
    return nullptr;
  }
}

}  // namespace

struct JsonDocument::Impl {
  Json root;
};

JsonDocument::JsonDocument() : impl_(std::make_unique<Impl>()) {}
JsonDocument::~JsonDocument() = default;
JsonDocument::JsonDocument(JsonDocument&&) noexcept = default;
JsonDocument& JsonDocument::operator=(JsonDocument&&) noexcept = default;

JsonParseError JsonDocument::parseFile(const std::string& path, std::size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return JsonParseError::FileNotFound;
  }

  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) {
    return JsonParseError::ReadError;
  }
  if (static_cast<std::size_t>(size) > max_bytes) {
    return JsonParseError::TooLarge;
  }
  in.seekg(0, std::ios::beg);

  std::string text(static_cast<std::size_t>(size), '\0');
  in.read(text.data(), size);
  if (!in && !in.eof()) {
    return JsonParseError::ReadError;
  }

  return parseString(text);
}

JsonParseError JsonDocument::parseString(const std::string& text) {
  // Non-throwing parse: a malformed configuration file is an expected input,
  // and the caller reports it as a LoadError rather than propagating.
  Json parsed = Json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return JsonParseError::ParseError;
  }
  impl_->root = std::move(parsed);
  return JsonParseError::None;
}

std::vector<std::string> JsonDocument::leafPointers() const {
  std::vector<std::string> out;
  collectLeaves(impl_->root, "", out);
  return out;
}

std::optional<std::int64_t> JsonDocument::intAt(const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  // is_number_integer() is false for 1024.0 and for "1024": a type-strict read
  // is what lets the loader report wrong_type separately from out_of_range.
  if (node == nullptr || !node->is_number_integer()) {
    return std::nullopt;
  }
  return node->get<std::int64_t>();
}

std::optional<bool> JsonDocument::boolAt(const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  if (node == nullptr || !node->is_boolean()) {
    return std::nullopt;
  }
  return node->get<bool>();
}

std::optional<std::string> JsonDocument::stringAt(const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  if (node == nullptr || !node->is_string()) {
    return std::nullopt;
  }
  return node->get<std::string>();
}

std::optional<double> JsonDocument::numberAt(const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  if (node == nullptr || !node->is_number()) {
    return std::nullopt;
  }
  return node->get<double>();
}

std::optional<std::vector<std::int64_t>> JsonDocument::intArrayAt(
    const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  if (node == nullptr || !node->is_array()) {
    return std::nullopt;
  }
  std::vector<std::int64_t> out;
  out.reserve(node->size());
  for (const auto& element : *node) {
    if (!element.is_number_integer()) {
      return std::nullopt;
    }
    out.push_back(element.get<std::int64_t>());
  }
  return out;
}

std::vector<std::string> JsonDocument::objectKeysAt(const std::string& pointer) const {
  const Json* node = at(impl_->root, pointer);
  if (node == nullptr || !node->is_object()) {
    return {};
  }
  std::vector<std::string> out;
  out.reserve(node->size());
  for (auto it = node->begin(); it != node->end(); ++it) {
    out.push_back(it.key());
  }
  return out;
}

bool JsonDocument::has(const std::string& pointer) const {
  return at(impl_->root, pointer) != nullptr;
}

}  // namespace tcu::can
