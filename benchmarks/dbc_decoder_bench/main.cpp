// NFR-03 and NFR-04, reported and never asserted (research R-07).
//
// Spec section 4.2 calls its numbers "goals for the lab, measure a baseline and
// adjust by CPU/OS; no hard real-time promise". So this prints percentiles and
// the environment they were measured in, and exits 0 whatever they are. A
// latency assertion on a shared machine fails for reasons that have nothing to
// do with this code, and a flaky gate teaches people to ignore gates.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

using Clock = std::chrono::steady_clock;

const char* kLabDbc =
    "VERSION \"bench\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
    " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n"
    " SG_ CabinTemp : 24|8@1- (1,-40) [-40|125] \"degC\" TCU\n"
    "BO_ 257 TORQUE_STATUS: 8 ECU\n"
    " SG_ Torque : 7|12@0- (0.5,0) [-1024|1023.5] \"Nm\" TCU\n"
    "BO_ 258 MUX_STATUS: 8 ECU\n"
    " SG_ Page M : 0|8@1+ (1,0) [0|255] \"\" TCU\n"
    " SG_ CommonStatus : 8|8@1+ (1,0) [0|255] \"\" TCU\n"
    " SG_ BatteryVoltage m1 : 16|16@1+ (0.01,0) [0|50] \"V\" TCU\n"
    " SG_ MotorTemp m2 : 16|16@1- (0.1,0) [-100|100] \"degC\" TCU\n";

struct Percentiles {
  double p50{0};
  double p99{0};
  double max{0};
  double mean{0};
};

Percentiles summarize(std::vector<double>& samples) {
  Percentiles out;
  if (samples.empty()) return out;
  std::sort(samples.begin(), samples.end());
  const auto at = [&samples](double fraction) {
    const std::size_t index =
        static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
  };
  out.p50 = at(0.50);
  out.p99 = at(0.99);
  out.max = samples.back();
  double total = 0;
  for (const double sample : samples) total += sample;
  out.mean = total / static_cast<double>(samples.size());
  return out;
}

void report(const char* what, Percentiles p, const char* unit, const char* target) {
  std::printf("  %-34s mean %8.2f  p50 %8.2f  p99 %8.2f  max %9.2f %s   (%s)\n", what, p.mean,
              p.p50, p.p99, p.max, unit, target);
}

std::string buildType() {
#if defined(NDEBUG)
  return "NDEBUG (optimized)";
#else
  return "debug (assertions on, NOT a performance build)";
#endif
}

std::string compiler() {
#if defined(__clang__)
  return std::string("clang ") + __VERSION__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

std::string cpuModel() {
  std::FILE* info = std::fopen("/proc/cpuinfo", "r");
  if (info == nullptr) return "unknown";
  char line[512];
  std::string model = "unknown";
  while (std::fgets(line, sizeof(line), info) != nullptr) {
    const std::string text(line);
    if (text.rfind("model name", 0) != 0) continue;
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) continue;
    model = text.substr(colon + 2);
    if (!model.empty() && model.back() == 10) model.pop_back();
    break;
  }
  std::fclose(info);
  return model;
}

/// A generated database, for NFR-04 at a realistic size.
std::string largeDbc(std::size_t messages) {
  std::string text = "VERSION \"large\"\n";
  for (std::size_t i = 0; i < messages; ++i) {
    // Extended identifiers: a standard id above 0x7FF is correctly rejected by
    // our own validator, so a large generated database has to use 29-bit ids.
    const std::uint64_t rawId = 0x80000000ull + 256ull + i;
    text += "BO_ " + std::to_string(rawId) + " M" + std::to_string(i) + ": 8 ECU\n";
    text += " SG_ A : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n";
    text += " SG_ B : 16|16@1- (0.5,0) [-100|100] \"Nm\" TCU\n";
    text += " SG_ C : 32|8@1+ (1,0) [0|255] \"\" TCU\n";
  }
  return text;
}

}  // namespace

int main() {
  std::printf("dbc_decoder_bench\n");
  std::printf("  cpu       : %s\n", cpuModel().c_str());
  std::printf("  compiler  : %s\n", compiler().c_str());
  std::printf("  build     : %s\n", buildType().c_str());
  std::printf("\nNumbers are REPORTED, not asserted: spec section 4.2 calls them lab goals.\n\n");

  const auto loaded = tcu::dbc::loadDbcBytes(kLabDbc, "bench.dbc");
  if (!loaded.ok()) {
    std::fprintf(stderr, "the benchmark DBC did not load\n");
    return 1;
  }
  const tcu::dbc::SignalDecoder decoder(loaded.db, tcu::dbc::DecodePolicy{});

  struct Frame {
    const char* name;
    std::uint32_t id;
    std::array<std::uint8_t, 8> payload;
  };
  const Frame frames[] = {
      {"decode VEHICLE_STATUS (3 Intel)", 256, {0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0}},
      {"decode TORQUE_STATUS (Motorola)", 257, {0xAB, 0xC0, 0, 0, 0, 0, 0, 0}},
      {"decode MUX_STATUS (page 1)", 258, {0x01, 0x7F, 0xD2, 0x04, 0, 0, 0, 0}},
  };

  constexpr int kIterations = 200000;
  std::printf("pure decode, %d iterations each:\n", kIterations);
  for (const auto& frame : frames) {
    tcu::dbc::CanFrameView view;
    view.arbitrationId = frame.id;
    view.data = frame.payload.data();
    view.size = 8;

    for (int i = 0; i < 1000; ++i) (void)decoder.decode(view);

    std::vector<double> samples;
    samples.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i) {
      const auto began = Clock::now();
      const auto result = decoder.decode(view);
      const auto elapsed = Clock::now() - began;
      if (result.error != tcu::dbc::DecodeError::None) {
        std::fprintf(stderr, "%s failed to decode\n", frame.name);
        return 1;
      }
      samples.push_back(
          std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsed).count());
    }
    report(frame.name, summarize(samples), "us", "NFR-03 target: p99 < 200 us");
  }

  std::printf("\ndbc_load:\n");
  const std::size_t sizes[] = {3, 500, 2000};
  for (const std::size_t messages : sizes) {
    const std::string text = messages == 3 ? std::string(kLabDbc) : largeDbc(messages);
    std::vector<double> samples;
    const int runs = messages > 500 ? 5 : 50;
    for (int i = 0; i < runs; ++i) {
      const auto began = Clock::now();
      const auto result = tcu::dbc::loadDbcBytes(text, "bench.dbc");
      const auto elapsed = Clock::now() - began;
      if (!result.ok()) {
        std::fprintf(stderr, "the generated DBC did not load\n");
        return 1;
      }
      samples.push_back(
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count());
    }
    const std::string label = "load " + std::to_string(messages) + " messages";
    report(label.c_str(), summarize(samples), "ms", "NFR-04 target: < 500 ms");
  }

  std::printf("\nNot measured here: ingress-to-publish latency, which needs the service and\n");
  std::printf("therefore a transport; and CPU or memory on any target SoC, which spec\n");
  std::printf("section 24 question 7 leaves open.\n");
  return 0;
}
