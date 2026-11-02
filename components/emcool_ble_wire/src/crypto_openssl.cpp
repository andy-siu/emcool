// The host backend of crypto.hpp, on OpenSSL 3's EVP interfaces.

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "emcool/ble/crypto.hpp"

namespace emcool::ble {

namespace {

struct MacDeleter {
  void operator()(EVP_MAC* mac) const { EVP_MAC_free(mac); }
};
struct MacContextDeleter {
  void operator()(EVP_MAC_CTX* context) const { EVP_MAC_CTX_free(context); }
};
struct CipherContextDeleter {
  void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};

int length(std::size_t size) { return static_cast<int>(size); }

}  // namespace

bool initCrypto() { return true; }

bool hmacSha256(std::span<const std::uint8_t> key, std::initializer_list<std::span<const std::uint8_t>> parts,
                Sha256Digest& out) {
  std::unique_ptr<EVP_MAC, MacDeleter> mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr));
  if (!mac) return false;
  std::unique_ptr<EVP_MAC_CTX, MacContextDeleter> context(EVP_MAC_CTX_new(mac.get()));
  if (!context) return false;
  char digest[] = "SHA256";
  const OSSL_PARAM parameters[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
                                   OSSL_PARAM_construct_end()};
  if (EVP_MAC_init(context.get(), key.data(), key.size(), parameters) != 1) return false;
  for (const std::span<const std::uint8_t> part : parts) {
    if (EVP_MAC_update(context.get(), part.data(), part.size()) != 1) return false;
  }
  std::size_t written = 0;
  return EVP_MAC_final(context.get(), out.data(), &written, out.size()) == 1 && written == out.size();
}

bool aesGcmSeal(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> plaintext, std::span<std::uint8_t> out) {
  if (out.size() < plaintext.size() + kGcmTagBytes) return false;
  std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> context(EVP_CIPHER_CTX_new());
  if (!context) return false;
  int written = 0;
  if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) return false;
  if (!aad.empty() && EVP_EncryptUpdate(context.get(), nullptr, &written, aad.data(), length(aad.size())) != 1) {
    return false;
  }
  int total = 0;
  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(context.get(), out.data(), &written, plaintext.data(), length(plaintext.size())) != 1) {
      return false;
    }
    total = written;
  }
  if (EVP_EncryptFinal_ex(context.get(), out.data() + total, &written) != 1) return false;
  total += written;
  return EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, length(kGcmTagBytes), out.data() + total) == 1 &&
         static_cast<std::size_t>(total) == plaintext.size();
}

bool aesGcmOpen(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> sealed, std::span<std::uint8_t> out) {
  if (sealed.size() < kGcmTagBytes) return false;
  const std::size_t ciphertextBytes = sealed.size() - kGcmTagBytes;
  if (out.size() < ciphertextBytes) return false;
  std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter> context(EVP_CIPHER_CTX_new());
  if (!context) return false;
  int written = 0;
  if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) return false;
  if (!aad.empty() && EVP_DecryptUpdate(context.get(), nullptr, &written, aad.data(), length(aad.size())) != 1) {
    return false;
  }
  int total = 0;
  if (ciphertextBytes > 0) {
    if (EVP_DecryptUpdate(context.get(), out.data(), &written, sealed.data(), length(ciphertextBytes)) != 1) {
      return false;
    }
    total = written;
  }
  // The tag is set before Final, which is where OpenSSL checks it.
  std::uint8_t tag[kGcmTagBytes];
  std::copy(sealed.end() - kGcmTagBytes, sealed.end(), tag);
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, length(kGcmTagBytes), tag) != 1) return false;
  return EVP_DecryptFinal_ex(context.get(), out.data() + total, &written) == 1;
}

void fillRandom(std::span<std::uint8_t> bytes) {
  if (RAND_bytes(bytes.data(), length(bytes.size())) != 1) {
    // No randomness means no safe nonce; there is nothing sensible to continue with.
    std::abort();
  }
}

}  // namespace emcool::ble
