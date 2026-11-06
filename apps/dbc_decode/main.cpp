// dbc_decode: decode one hex frame or a candump capture against a DBC file.
//
// Deterministic for identical input: no wall clock, no map iteration in the
// output, no locale-dependent formatting (NFR-09). Exit codes per
// contracts/dbc_api.md section 5: 0 all frames decoded, 1 a frame produced a
// DecodeError, 2 usage or I/O error.
//
// The candump reader lives here, not in tcu_signal: it is a CLI convenience,
// not a transport. Live frames come from project 05's ICanTransport
// (research R-22).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

struct RawFrame {
  std::uint32_t id{0};
  bool isExtended{false};
  std::array<std::uint8_t, 8> data{};
  std::uint8_t size{0};
};

int usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s --dbc <file.dbc> (--hex <id>:<bytes> | --candump <file>) [--json]\n",
               program);
  return 2;
}

bool hexNibble(char c, std::uint8_t& out) {
  if (c >= '0' && c <= '9') { out = static_cast<std::uint8_t>(c - '0'); return true; }
  if (c >= 'a' && c <= 'f') { out = static_cast<std::uint8_t>(c - 'a' + 10); return true; }
  if (c >= 'A' && c <= 'F') { out = static_cast<std::uint8_t>(c - 'A' + 10); return true; }
  return false;
}

/// "100:1027010050000000" or "1F334455:AABB". An identifier of more than three
/// hex digits is read as extended, which is the candump convention.
bool parseHexFrame(const std::string& text, RawFrame& frame) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  const std::string idPart = text.substr(0, colon);
  const std::string dataPart = text.substr(colon + 1);
  if (idPart.size() > 8 || (dataPart.size() % 2u) != 0u || dataPart.size() > 16) return false;

  std::uint32_t id = 0;
  for (const char c : idPart) {
    std::uint8_t nibble = 0;
    if (!hexNibble(c, nibble)) return false;
    id = (id << 4) | nibble;
  }
  frame.id = id;
  frame.isExtended = idPart.size() > 3;
  frame.size = static_cast<std::uint8_t>(dataPart.size() / 2u);
  for (std::uint8_t i = 0; i < frame.size; ++i) {
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    if (!hexNibble(dataPart[i * 2u], high) || !hexNibble(dataPart[i * 2u + 1u], low)) return false;
    frame.data[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

/// candump text: "(1730793600.000000) vcan0 100#1027010050000000". A line that
/// does not parse is skipped and counted, never guessed at.
std::size_t readCandump(const std::string& path, std::vector<RawFrame>& frames, bool& opened) {
  std::ifstream in(path);
  opened = static_cast<bool>(in);
  if (!opened) return 0;

  std::size_t skipped = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::size_t hash = line.find('#');
    if (hash == std::string::npos) { ++skipped; continue; }
    std::size_t idStart = line.rfind(' ', hash);
    idStart = (idStart == std::string::npos) ? 0 : idStart + 1;
    const std::string spec = line.substr(idStart, hash - idStart) + ":" + line.substr(hash + 1);
    RawFrame frame;
    if (parseHexFrame(spec, frame)) {
      frames.push_back(frame);
    } else {
      ++skipped;
    }
  }
  return skipped;
}

void printText(const tcu::dbc::DecodedBatch& batch) {
  std::fprintf(stdout, "%s (0x%X, %s) version %llu\n", batch.messageName.c_str(),
               batch.arbitrationId,
               batch.format == tcu::dbc::FrameFormat::Extended29 ? "EFF" : "STD",
               static_cast<unsigned long long>(batch.dbcVersion));
  if (batch.muxUnresolved) {
    std::fprintf(stdout, "  (multiplex selector matched no branch; no branch was guessed)\n");
  }
  for (const auto& value : batch.signals) {
    std::fprintf(stdout, "  %-16s raw=0x%llX", value.signalName.c_str(),
                 static_cast<unsigned long long>(value.raw));
    if (value.signedRaw.has_value()) {
      std::fprintf(stdout, " signed=%lld", static_cast<long long>(*value.signedRaw));
    }
    std::fprintf(stdout, " physical=%.6g", value.physical);
    if (!value.unit.empty()) std::fprintf(stdout, " %s", value.unit.c_str());
    if (value.enumLabel.has_value()) std::fprintf(stdout, " [%s]", value.enumLabel->c_str());
    std::fprintf(stdout, " quality=%s", tcu::dbc::toString(value.quality));
    if (value.physicalPrecisionLoss) std::fprintf(stdout, " (physical rounded)");
    std::fprintf(stdout, "\n");
  }
}

void printJson(const tcu::dbc::DecodedBatch& batch) {
  std::fprintf(stdout,
               "{\"message\":\"%s\",\"id\":%u,\"format\":\"%s\",\"dbc_version\":%llu,"
               "\"dbc_hash\":\"%s\",\"mux_unresolved\":%s,\"signals\":[",
               batch.messageName.c_str(), batch.arbitrationId,
               batch.format == tcu::dbc::FrameFormat::Extended29 ? "EFF" : "STD",
               static_cast<unsigned long long>(batch.dbcVersion), batch.dbcHash.c_str(),
               batch.muxUnresolved ? "true" : "false");
  for (std::size_t i = 0; i < batch.signals.size(); ++i) {
    const auto& value = batch.signals[i];
    std::fprintf(stdout,
                 "%s{\"name\":\"%s\",\"raw\":%llu,\"physical\":%.17g,\"unit\":\"%s\","
                 "\"quality\":\"%s\"}",
                 i == 0 ? "" : ",", value.signalName.c_str(),
                 static_cast<unsigned long long>(value.raw), value.physical, value.unit.c_str(),
                 tcu::dbc::toString(value.quality));
  }
  std::fprintf(stdout, "]}\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dbcPath;
  std::string candumpPath;
  std::vector<std::string> hexFrames;
  bool json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      json = true;
    } else if (arg == "--dbc" && i + 1 < argc) {
      dbcPath = argv[++i];
    } else if (arg == "--hex" && i + 1 < argc) {
      hexFrames.emplace_back(argv[++i]);
    } else if (arg == "--candump" && i + 1 < argc) {
      candumpPath = argv[++i];
    } else {
      return usage(argv[0]);
    }
  }
  if (dbcPath.empty() || (hexFrames.empty() && candumpPath.empty())) return usage(argv[0]);

  const tcu::dbc::LoadResult loaded = tcu::dbc::loadDbcFile(dbcPath);
  if (!loaded.ok()) {
    for (const auto& diagnostic : loaded.diagnostics) {
      if (diagnostic.severity != tcu::dbc::Severity::Error) continue;
      std::fprintf(stderr, "%s:%u:%u: %s: %s\n", diagnostic.file.c_str(), diagnostic.line,
                   diagnostic.column, tcu::dbc::toString(diagnostic.code),
                   diagnostic.message.c_str());
    }
    return 2;
  }

  std::vector<RawFrame> frames;
  for (const auto& spec : hexFrames) {
    RawFrame frame;
    if (!parseHexFrame(spec, frame)) {
      std::fprintf(stderr, "cannot parse --hex argument\n");
      return 2;
    }
    frames.push_back(frame);
  }
  if (!candumpPath.empty()) {
    bool opened = false;
    const std::size_t skipped = readCandump(candumpPath, frames, opened);
    if (!opened) {
      std::fprintf(stderr, "cannot open the candump file\n");
      return 2;
    }
    if (skipped > 0) std::fprintf(stderr, "skipped %zu unparsable line(s)\n", skipped);
  }

  tcu::dbc::SignalDecoder decoder(loaded.db, tcu::dbc::DecodePolicy{});
  bool anyError = false;
  for (const auto& raw : frames) {
    tcu::dbc::CanFrameView view;
    view.arbitrationId = raw.id;
    view.format = raw.isExtended ? tcu::dbc::FrameFormat::Extended29
                                 : tcu::dbc::FrameFormat::Standard11;
    view.data = raw.data.data();
    view.size = raw.size;
    // No wall clock: a CLI run must be byte-identical twice in a row (NFR-09).
    view.rxTime = std::chrono::steady_clock::time_point{};

    const auto result = decoder.decode(view);
    if (result.error != tcu::dbc::DecodeError::None) {
      anyError = true;
      std::fprintf(stdout, "0x%X: %s\n", raw.id, tcu::dbc::toString(result.error));
      continue;
    }
    if (json) {
      printJson(*result.batch);
    } else {
      printText(*result.batch);
    }
  }
  return anyError ? 1 : 0;
}
