// T051: race recipe A at its full iteration count (spec section 15.3).
//
// The unit suite covers the barrier once. This runs the pin-decode-swap
// interleaving repeatedly without a file write per iteration, which is what
// makes 10,000 iterations affordable: the hazard is in the pointer handover,
// not in the parsing, so the loop swaps between two pre-built databases.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

namespace {

const char* kV1 =
    "VERSION \"v1\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n";

const char* kV2 =
    "VERSION \"v2\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
    " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n";

std::shared_ptr<const tcu::dbc::CompiledDbc> rebuild(
    const std::shared_ptr<const tcu::dbc::CompiledDbc>& from, std::uint64_t version) {
  return std::make_shared<const tcu::dbc::CompiledDbc>(version, from->hash(), from->sourcePath(),
                                                       from->messages(), from->stats());
}

TEST(RaceA_InFlightFramePinsOldVersion, TenThousandSwapsNeverMixAVersion) {
  const auto loadedV1 = tcu::dbc::loadDbcBytes(kV1, "v1.dbc");
  const auto loadedV2 = tcu::dbc::loadDbcBytes(kV2, "v2.dbc");
  ASSERT_TRUE(loadedV1.ok());
  ASSERT_TRUE(loadedV2.ok());

  // Two databases, alternating versions, so a decode that pinned one can be
  // checked against the identity it carries.
  std::vector<std::shared_ptr<const tcu::dbc::CompiledDbc>> generations;
  for (std::uint64_t version = 1; version <= 2; ++version) {
    generations.push_back(rebuild(version == 1 ? loadedV1.db : loadedV2.db, version));
  }

  std::shared_ptr<const tcu::dbc::CompiledDbc> published = generations[0];
  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> swaps{0};

  // The reloader: nothing but atomic stores, exactly what the coordinator does
  // at its commit point (research R-14).
  std::thread reloader([&] {
    std::uint64_t index = 0;
    while (running.load(std::memory_order_relaxed)) {
      std::atomic_store(&published, generations[index % generations.size()]);
      ++index;
      swaps.fetch_add(1, std::memory_order_relaxed);
    }
  });

  const std::uint8_t payload[8] = {0x10, 0x27, 0x01, 0, 0, 0, 0, 0};
  std::uint64_t decoded = 0;
  for (int iteration = 0; iteration < 10000; ++iteration) {
    // Pinned ONCE, as the worker does, and used for the whole frame.
    const std::shared_ptr<const tcu::dbc::CompiledDbc> pinned = std::atomic_load(&published);
    ASSERT_NE(pinned, nullptr);
    const std::uint64_t pinnedVersion = pinned->version();
    const std::string pinnedHash = pinned->hash();

    tcu::dbc::CanFrameView frame;
    frame.arbitrationId = 256;
    frame.data = payload;
    frame.size = 8;

    const tcu::dbc::SignalDecoder decoder(pinned, tcu::dbc::DecodePolicy{});
    const auto result = decoder.decode(frame);
    ASSERT_EQ(result.error, tcu::dbc::DecodeError::None) << iteration;
    ASSERT_TRUE(result.batch.has_value()) << iteration;

    // The batch must carry the identity of the database it pinned, whatever the
    // reloader did in the meantime (invariant P-13, NFR-06, B-12).
    ASSERT_EQ(result.batch->dbcVersion, pinnedVersion) << "iteration " << iteration;
    ASSERT_EQ(result.batch->dbcHash, pinnedHash) << "iteration " << iteration;
    ++decoded;
  }

  running.store(false);
  reloader.join();

  EXPECT_EQ(decoded, 10000u);
  EXPECT_GT(swaps.load(), 0u) << "the reloader never ran, so nothing was raced";
}

TEST(RaceA_InFlightFramePinsOldVersion, APinnedDatabaseOutlivesItsReplacement) {
  // The other half of the handover: a worker holding the old pointer must keep
  // a valid object after the swap, which is what shared_ptr buys us here.
  const auto loaded = tcu::dbc::loadDbcBytes(kV1, "v1.dbc");
  ASSERT_TRUE(loaded.ok());
  std::shared_ptr<const tcu::dbc::CompiledDbc> published = rebuild(loaded.db, 1);

  const std::shared_ptr<const tcu::dbc::CompiledDbc> pinned = std::atomic_load(&published);
  std::atomic_store(&published, rebuild(loaded.db, 2));

  // The replacement is live, and the pinned one is still fully usable.
  EXPECT_EQ(std::atomic_load(&published)->version(), 2u);
  EXPECT_EQ(pinned->version(), 1u);
  ASSERT_NE(pinned->find(0, tcu::dbc::FrameFormat::Standard11, 256), nullptr);
  EXPECT_EQ(pinned->find(0, tcu::dbc::FrameFormat::Standard11, 256)->name, "VEHICLE_STATUS");
}

}  // namespace
