#include "tcu/signal/decoder_metrics.hpp"

namespace tcu::signal {

const char* toString(FrameResult result) noexcept {
  switch (result) {
    case FrameResult::Ok:          return "ok";
    case FrameResult::UnknownId:   return "unknown_id";
    case FrameResult::WrongLength: return "wrong_length";
    case FrameResult::WrongFormat: return "wrong_format";
    case FrameResult::ErrorFrame:  return "error_frame";
    case FrameResult::RemoteFrame: return "remote_frame";
    case FrameResult::DecodeError: return "decode_error";
    case FrameResult::Count:       break;
  }
  return "decode_error";
}

const char* toString(LoadResultLabel result) noexcept {
  switch (result) {
    case LoadResultLabel::Ok:          return "ok";
    case LoadResultLabel::Rejected:    return "rejected";
    case LoadResultLabel::Busy:        return "busy";
    case LoadResultLabel::RateLimited: return "rate_limited";
    case LoadResultLabel::SourceError: return "source_error";
    case LoadResultLabel::Count:       break;
  }
  return "rejected";
}

QualityLabel toLabel(dbc::SignalQuality quality) noexcept {
  switch (quality) {
    case dbc::SignalQuality::NotReceived:       return QualityLabel::NotReceived;
    case dbc::SignalQuality::Valid:             return QualityLabel::Valid;
    case dbc::SignalQuality::InvalidRaw:        return QualityLabel::InvalidRaw;
    case dbc::SignalQuality::OutOfRange:        return QualityLabel::OutOfRange;
    case dbc::SignalQuality::Stale:             return QualityLabel::Stale;
    case dbc::SignalQuality::DecodeError:       return QualityLabel::DecodeError;
    case dbc::SignalQuality::NotActive:         return QualityLabel::NotActive;
    case dbc::SignalQuality::ConfigUnavailable: return QualityLabel::ConfigUnavailable;
  }
  return QualityLabel::NotReceived;
}

DecoderMetricsSnapshot DecoderMetrics::snapshot() const {
  DecoderMetricsSnapshot out;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    out.frames[i] = frames_[i].load(std::memory_order_relaxed);
  }
  for (std::size_t i = 0; i < loadAttempts_.size(); ++i) {
    out.loadAttempts[i] = loadAttempts_[i].load(std::memory_order_relaxed);
  }
  for (std::size_t i = 0; i < signals_.size(); ++i) {
    out.signals[i] = signals_[i].load(std::memory_order_relaxed);
  }
  out.framesReceived = framesReceived.load(std::memory_order_relaxed);
  out.batchesPublished = batchesPublished.load(std::memory_order_relaxed);
  out.batchesDroppedByPolicy = batchesDroppedByPolicy.load(std::memory_order_relaxed);
  out.staleTransitions = staleTransitions.load(std::memory_order_relaxed);
  out.recoveredTransitions = recoveredTransitions.load(std::memory_order_relaxed);
  out.unknownMux = unknownMux.load(std::memory_order_relaxed);
  out.observerFailures = observerFailures.load(std::memory_order_relaxed);
  out.eventPublishFailures = eventPublishFailures.load(std::memory_order_relaxed);
  out.schemaTransitions = schemaTransitions.load(std::memory_order_relaxed);
  out.logsSuppressed = logsSuppressed.load(std::memory_order_relaxed);
  out.activeVersion = activeVersion.load(std::memory_order_relaxed);
  return out;
}

}  // namespace tcu::signal
