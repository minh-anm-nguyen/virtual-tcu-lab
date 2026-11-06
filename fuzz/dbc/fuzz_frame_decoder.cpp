// Fuzz target: the frame decoder (NFR-05, research R-21).
//
// The database is fixed - the parser has its own target - and the fuzzer drives
// the frame: identifier, format, flags, length and payload. The invariant is
// that decode() never throws and never reports a signal as Valid from a frame
// it refused.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

const char* kFuzzDbc =
    "VERSION \"fuzz\"\n"
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

const tcu::dbc::SignalDecoder& decoder() {
  static const tcu::dbc::SignalDecoder* instance = [] {
    const auto loaded = tcu::dbc::loadDbcBytes(kFuzzDbc, "fuzz-db.dbc");
    if (!loaded.ok()) __builtin_trap();
    return new tcu::dbc::SignalDecoder(loaded.db, tcu::dbc::DecodePolicy{});
  }();
  return *instance;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Four control bytes, then the payload.
  if (size < 5) return 0;

  tcu::dbc::CanFrameView frame;
  frame.arbitrationId = (static_cast<std::uint32_t>(data[0]) << 8) | data[1];
  frame.format = (data[2] & 1u) != 0 ? tcu::dbc::FrameFormat::Extended29
                                     : tcu::dbc::FrameFormat::Standard11;
  frame.isRemote = (data[2] & 2u) != 0;
  frame.isError = (data[2] & 4u) != 0;
  // Deliberately allowed to exceed the real payload: the length check must
  // refuse it rather than the extractor reading past the buffer (invariant P-02).
  frame.size = data[3];
  frame.busId = data[2] >> 3;

  const std::uint8_t* payload = data + 4;
  const std::size_t available = size - 4;
  if (frame.size > available) frame.size = static_cast<std::uint8_t>(available);
  frame.data = payload;

  const tcu::dbc::DecodeResult result = decoder().decode(frame);

  if (result.error != tcu::dbc::DecodeError::None) {
    // A refused frame yields no batch at all: there is no partial answer a
    // consumer could mistake for data (invariants P-11, P-12).
    if (result.batch.has_value()) __builtin_trap();
    return 0;
  }
  if (!result.batch.has_value()) __builtin_trap();

  // Exactly one schema identity, and no signal claiming Valid with a
  // non-finite physical value (invariants P-05, P-13).
  if (result.batch->dbcVersion == 0) __builtin_trap();
  if (result.batch->dbcHash.size() != 64) __builtin_trap();
  for (const auto& value : result.batch->signals) {
    if (value.quality != tcu::dbc::SignalQuality::Valid) continue;
    if (!(value.physical == value.physical)) __builtin_trap();  // NaN
    if (value.isSigned != value.signedRaw.has_value()) __builtin_trap();
  }
  return 0;
}
