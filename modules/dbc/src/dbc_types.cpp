#include "tcu/dbc/frame_view.hpp"
#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc {

const char* toString(DbcErrorCode code) noexcept {
  switch (code) {
    case DbcErrorCode::None:               return "NONE";
    case DbcErrorCode::DbcSyntax:          return "DBC_SYNTAX";
    case DbcErrorCode::DbcLimit:           return "DBC_LIMIT";
    case DbcErrorCode::DbcMessage:         return "DBC_MESSAGE";
    case DbcErrorCode::DbcSignal:          return "DBC_SIGNAL";
    case DbcErrorCode::DbcOutOfBounds:     return "DBC_OUT_OF_BOUNDS";
    case DbcErrorCode::DbcOverlap:         return "DBC_OVERLAP";
    case DbcErrorCode::DbcMux:             return "DBC_MUX";
    case DbcErrorCode::DbcEnum:            return "DBC_ENUM";
    case DbcErrorCode::DbcRange:           return "DBC_RANGE";
    case DbcErrorCode::UnsupportedFeature: return "UNSUPPORTED_FEATURE";
    case DbcErrorCode::DbcSource:          return "DBC_SOURCE";
  }
  return "NONE";
}

}  // namespace tcu::dbc

namespace tcu::dbc {

const char* toString(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::None:               return "ok";
    case DecodeError::UnknownMessage:     return "unknown_id";
    case DecodeError::WrongFormat:        return "wrong_format";
    case DecodeError::WrongLength:        return "wrong_length";
    case DecodeError::ErrorFrame:         return "error_frame";
    case DecodeError::RemoteFrame:        return "remote_frame";
    case DecodeError::UnsupportedFeature: return "unsupported_feature";
    case DecodeError::InvalidBitLayout:   return "invalid_bit_layout";
    case DecodeError::NonFinitePhysical:  return "non_finite_physical";
    case DecodeError::InternalError:      return "internal_error";
  }
  return "internal_error";
}

}  // namespace tcu::dbc
