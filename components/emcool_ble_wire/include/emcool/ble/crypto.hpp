#pragma once

// The three primitives the phone link needs: HMAC-SHA256, AES-256-GCM and random bytes.
// mbedTLS's PSA API on the ESP32, OpenSSL on the host: one contract, two backends. Both are
// checked against published test vectors by cryptoSelfTest(), which the firmware runs before
// it will advertise, and the host backend is checked against the phone's own WebCrypto by
// tools/check-ble.mts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace emcool::ble {

using Sha256Digest = std::array<std::uint8_t, 32>;
using AesKey = std::array<std::uint8_t, 32>;
using GcmIv = std::array<std::uint8_t, 12>;
inline constexpr std::size_t kGcmTagBytes = 16;

bool initCrypto();

// HMAC-SHA256 over the parts, concatenated.
bool hmacSha256(std::span<const std::uint8_t> key, std::initializer_list<std::span<const std::uint8_t>> parts,
                Sha256Digest& out);

// Writes ciphertext then the 16-byte tag — WebCrypto's layout — so `out` must hold
// plaintext.size() + kGcmTagBytes. `out` must not overlap `plaintext`. The plaintext must not be
// empty: ESP-IDF 6.1's GCM refuses one, so both backends do. The link never seals one, since a
// sealed message is deflated first and raw deflate is never empty.
bool aesGcmSeal(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> plaintext, std::span<std::uint8_t> out);

// `sealed` is ciphertext then tag; `out` must hold sealed.size() - kGcmTagBytes. False when
// the tag doesn't verify (wrong key, or tampering), and for a tag alone, an empty message.
bool aesGcmOpen(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> sealed, std::span<std::uint8_t> out);

void fillRandom(std::span<std::uint8_t> bytes);

// RFC 4231 test case 2 and GCM test cases 16 and 14 (McGrew & Viega) through the functions
// above, including a tampered tag that must be refused.
bool cryptoSelfTest();

}  // namespace emcool::ble
