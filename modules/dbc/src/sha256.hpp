#pragma once

// FIPS 180-4 SHA-256, streaming.
//
// Spec section 6.3 names SHA-256 for dbc_hash, so std::hash is not an option -
// it is not a digest and is not stable across runs. OpenSSL would add a linked
// system dependency to a library that otherwise has none, for one call
// (research R-19). This is a content fingerprint, NOT a security boundary:
// research R-08 states there is no signature and no provenance check in v1.
//
// Verified against the published NIST vectors in tests/unit/dbc/sha256_test.cpp.

#include <cstddef>
#include <cstdint>
#include <string>

namespace tcu::dbc::detail {

class Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t length) noexcept;

  /// Lower-case hex, 64 characters. Finalizes: do not update() afterwards.
  std::string finalHex() noexcept;

  /// Convenience for a whole buffer.
  static std::string hexOf(const void* data, std::size_t length) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint8_t buffer_[64];
  std::size_t bufferLength_{0};
  std::uint64_t totalBits_{0};
};

}  // namespace tcu::dbc::detail
