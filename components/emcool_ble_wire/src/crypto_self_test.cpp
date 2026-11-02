#include <cstring>

#include "emcool/ble/crypto.hpp"
#include "emcool/log.hpp"

namespace emcool::ble {

namespace {

constexpr const char* kTag = "ble-crypto";

// RFC 4231 §4.3, test case 2.
constexpr std::uint8_t kHmacExpected[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                                            0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                                            0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
// GCM spec test case 13: all-zero 256-bit key and IV, empty plaintext.
constexpr std::uint8_t kGcmEmptyTag[16] = {0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9,
                                           0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb, 0x73, 0x8b};
// Test case 14: the same with 16 zero bytes of plaintext — ciphertext, then tag.
constexpr std::uint8_t kGcmBlockSealed[32] = {0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e, 0x07, 0x4e, 0xc5,
                                              0xd3, 0xba, 0xf3, 0x9d, 0x18, 0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99,
                                              0x6b, 0xf0, 0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};

std::span<const std::uint8_t> bytesOf(const char* text) {
  return {reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)};
}

}  // namespace

bool cryptoSelfTest() {
  Sha256Digest digest = {};
  if (!hmacSha256(bytesOf("Jefe"), {bytesOf("what do ya "), bytesOf("want for nothing?")}, digest) ||
      std::memcmp(digest.data(), kHmacExpected, sizeof kHmacExpected) != 0) {
    logError(kTag, "HMAC-SHA256 failed RFC 4231 test case 2");
    return false;
  }

  const AesKey zeroKey = {};
  const GcmIv zeroIv = {};
  std::uint8_t sealed[32] = {};
  if (!aesGcmSeal(zeroKey, zeroIv, {}, {}, std::span<std::uint8_t>(sealed, 16)) ||
      std::memcmp(sealed, kGcmEmptyTag, 16) != 0) {
    logError(kTag, "AES-256-GCM failed GCM test case 13");
    return false;
  }
  const std::uint8_t zeroBlock[16] = {};
  if (!aesGcmSeal(zeroKey, zeroIv, {}, zeroBlock, sealed) || std::memcmp(sealed, kGcmBlockSealed, 32) != 0) {
    logError(kTag, "AES-256-GCM failed GCM test case 14");
    return false;
  }
  std::uint8_t opened[16] = {0xff};
  if (!aesGcmOpen(zeroKey, zeroIv, {}, kGcmBlockSealed, opened) || std::memcmp(opened, zeroBlock, 16) != 0) {
    logError(kTag, "AES-256-GCM could not open GCM test case 14");
    return false;
  }
  std::uint8_t tampered[32];
  std::memcpy(tampered, kGcmBlockSealed, 32);
  tampered[31] ^= 0x01;
  if (aesGcmOpen(zeroKey, zeroIv, {}, tampered, opened)) {
    logError(kTag, "AES-256-GCM accepted a tampered tag");
    return false;
  }
  return true;
}

}  // namespace emcool::ble
