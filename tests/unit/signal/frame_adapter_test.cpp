// T045: project 05's CanMessage to the decoder's CanFrameView. The adapter is
// the whole transport integration, so its mapping is worth asserting field by
// field (research R-22).

#include <gtest/gtest.h>

#include <chrono>

#include "tcu/signal/frame_adapter.hpp"

namespace {

using tcu::can::CanMessage;
using tcu::can::FrameKind;

CanMessage labFrame() {
  CanMessage msg;
  msg.id = 0x100;
  msg.format = tcu::can::FrameFormat::Standard11;
  msg.kind = FrameKind::Data;
  msg.data = {0x10, 0x27, 0x01, 0x00, 0x50, 0x00, 0x00, 0x00};
  msg.length = 8;
  msg.received_at = tcu::can::SteadyTime{std::chrono::milliseconds{4321}};
  return msg;
}

TEST(FrameAdapter_MapsCanMessage, EveryFieldAndNoRestamping) {
  const CanMessage msg = labFrame();
  const auto view = tcu::signal::toFrameView(msg, 0);

  EXPECT_EQ(view.arbitrationId, 0x100u);
  EXPECT_EQ(view.format, tcu::dbc::FrameFormat::Standard11);
  EXPECT_EQ(view.size, 8u);
  EXPECT_FALSE(view.isRemote);
  EXPECT_FALSE(view.isError);
  EXPECT_EQ(view.busId, 0u);
  // The timestamp project 05 stamped is carried through untouched: this
  // project never decides what time it is (research R-06).
  EXPECT_EQ(view.rxTime, msg.received_at);
  // A borrowed view over the message's own array, not a copy.
  EXPECT_EQ(view.data, msg.data.data());
  EXPECT_EQ(view.data[1], 0x27u);
}

TEST(FrameAdapter_MapsRemoteAndErrorKinds, EachBecomesItsOwnFlag) {
  CanMessage remote = labFrame();
  remote.kind = FrameKind::Remote;
  const auto remoteView = tcu::signal::toFrameView(remote);
  EXPECT_TRUE(remoteView.isRemote);
  EXPECT_FALSE(remoteView.isError);

  CanMessage error = labFrame();
  error.kind = FrameKind::Error;
  const auto errorView = tcu::signal::toFrameView(error);
  EXPECT_TRUE(errorView.isError);
  EXPECT_FALSE(errorView.isRemote);
}

TEST(FrameAdapter_ExtendedFormatPreserved, NotFlattenedOntoStandard) {
  CanMessage msg = labFrame();
  msg.format = tcu::can::FrameFormat::Extended29;
  const auto view = tcu::signal::toFrameView(msg);
  EXPECT_EQ(view.format, tcu::dbc::FrameFormat::Extended29);
  EXPECT_EQ(view.arbitrationId, 0x100u) << "the identifier itself is unchanged";
}

TEST(FrameAdapter_MapsCanMessage, BusIdIsTheCallersChoice) {
  const CanMessage msg = labFrame();
  EXPECT_EQ(tcu::signal::toFrameView(msg, 3).busId, 3u);
}

TEST(FrameAdapter_MapsCanMessage, ZeroLengthFrameIsCarriedThrough) {
  CanMessage msg = labFrame();
  msg.length = 0;
  const auto view = tcu::signal::toFrameView(msg);
  EXPECT_EQ(view.size, 0u);
  EXPECT_NE(view.data, nullptr) << "the array still exists; only the length is zero";
}

}  // namespace
