// T013: the published FIPS 180-4 / NIST vectors. A content fingerprint nobody
// checked against an external oracle is not a fingerprint (research R-19).

#include <gtest/gtest.h>

#include <string>

#include "sha256.hpp"

namespace {

std::string hexOf(const std::string& text) {
  return tcu::dbc::detail::Sha256::hexOf(text.data(), text.size());
}

TEST(Sha256_NistVectors, EmptyString) {
  EXPECT_EQ(hexOf(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256_NistVectors, Abc) {
  EXPECT_EQ(hexOf("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256_NistVectors, FiftySixCharacters) {
  EXPECT_EQ(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256_NistVectors, OneMillionA) {
  tcu::dbc::detail::Sha256 hash;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) hash.update(chunk.data(), chunk.size());
  EXPECT_EQ(hash.finalHex(),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256_NistVectors, StreamingMatchesOneShot) {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  tcu::dbc::detail::Sha256 hash;
  for (char c : text) hash.update(&c, 1);
  EXPECT_EQ(hash.finalHex(), hexOf(text));
}

TEST(Sha256_NistVectors, OneByteChangeChangesDigest) {
  EXPECT_NE(hexOf("BO_ 256 VEHICLE_STATUS: 8 ECU"),
            hexOf("BO_ 257 VEHICLE_STATUS: 8 ECU"));
}

}  // namespace
