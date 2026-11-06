#pragma once

// Project 05's frame to the decoder's view of a frame.
//
// This is the whole transport integration (research R-22): project 05 owns the
// socket, the fake, the flag splitting and the length validation, so there is
// nothing to re-implement and no second frame-source interface. The view is
// BORROWED - it points into `msg`, so it must not outlive it. The RX queue owns
// the CanMessage and the worker builds a view over it for one decode.

#include <cstdint>

#include "tcu/can/can_types.hpp"
#include "tcu/dbc/frame_view.hpp"

namespace tcu::signal {

inline dbc::CanFrameView toFrameView(const tcu::can::CanMessage& msg,
                                     std::uint32_t busId = 0) noexcept {
  dbc::CanFrameView view;
  view.arbitrationId = msg.id;
  view.format = msg.format == tcu::can::FrameFormat::Extended29
                    ? dbc::FrameFormat::Extended29
                    : dbc::FrameFormat::Standard11;
  view.data = msg.data.data();
  view.size = msg.length;
  view.isRemote = msg.kind == tcu::can::FrameKind::Remote;
  view.isError = msg.kind == tcu::can::FrameKind::Error;
  // Project 05 stamped this from the injected clock the moment read() returned.
  // Nothing here re-stamps it (research R-06, spec section 10.5).
  view.rxTime = msg.received_at;
  view.busId = busId;
  return view;
}

}  // namespace tcu::signal
