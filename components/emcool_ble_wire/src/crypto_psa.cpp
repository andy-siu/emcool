// The ESP32 backend of crypto.hpp, on mbedTLS 4's PSA Crypto API (ESP-IDF v6.1). Keys are
// imported for each operation and destroyed straight after: a 32-byte copy, and no key
// slot is ever left holding a session key.

#include "emcool/ble/crypto.hpp"
#include "esp_random.h"
#include "psa/crypto.h"

namespace emcool::ble {

namespace {

class ImportedKey {
 public:
  ImportedKey(psa_key_type_t type, psa_key_usage_t usage, psa_algorithm_t algorithm,
              std::span<const std::uint8_t> material) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, type);
    psa_set_key_bits(&attributes, PSA_BYTES_TO_BITS(material.size()));
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, algorithm);
    imported_ = psa_import_key(&attributes, material.data(), material.size(), &id_) == PSA_SUCCESS;
  }
  ~ImportedKey() {
    if (imported_) {
      psa_destroy_key(id_);
    }
  }
  ImportedKey(const ImportedKey&) = delete;
  ImportedKey& operator=(const ImportedKey&) = delete;

  bool imported() const { return imported_; }
  psa_key_id_t id() const { return id_; }

 private:
  psa_key_id_t id_ = PSA_KEY_ID_NULL;
  bool imported_ = false;
};

}  // namespace

bool initCrypto() { return psa_crypto_init() == PSA_SUCCESS; }

bool hmacSha256(std::span<const std::uint8_t> key, std::initializer_list<std::span<const std::uint8_t>> parts,
                Sha256Digest& out) {
  const psa_algorithm_t algorithm = PSA_ALG_HMAC(PSA_ALG_SHA_256);
  ImportedKey imported(PSA_KEY_TYPE_HMAC, PSA_KEY_USAGE_SIGN_MESSAGE, algorithm, key);
  if (!imported.imported()) return false;
  psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;
  if (psa_mac_sign_setup(&operation, imported.id(), algorithm) != PSA_SUCCESS) return false;
  for (const std::span<const std::uint8_t> part : parts) {
    if (psa_mac_update(&operation, part.data(), part.size()) != PSA_SUCCESS) {
      psa_mac_abort(&operation);
      return false;
    }
  }
  std::size_t written = 0;
  if (psa_mac_sign_finish(&operation, out.data(), out.size(), &written) != PSA_SUCCESS) {
    psa_mac_abort(&operation);
    return false;
  }
  return written == out.size();
}

bool aesGcmSeal(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> plaintext, std::span<std::uint8_t> out) {
  if (plaintext.empty() || out.size() < plaintext.size() + kGcmTagBytes) return false;
  ImportedKey imported(PSA_KEY_TYPE_AES, PSA_KEY_USAGE_ENCRYPT, PSA_ALG_GCM, key);
  if (!imported.imported()) return false;
  std::size_t written = 0;
  return psa_aead_encrypt(imported.id(), PSA_ALG_GCM, iv.data(), iv.size(), aad.data(), aad.size(), plaintext.data(),
                          plaintext.size(), out.data(), out.size(), &written) == PSA_SUCCESS &&
         written == plaintext.size() + kGcmTagBytes;
}

bool aesGcmOpen(const AesKey& key, const GcmIv& iv, std::span<const std::uint8_t> aad,
                std::span<const std::uint8_t> sealed, std::span<std::uint8_t> out) {
  if (sealed.size() <= kGcmTagBytes || out.size() < sealed.size() - kGcmTagBytes) return false;
  ImportedKey imported(PSA_KEY_TYPE_AES, PSA_KEY_USAGE_DECRYPT, PSA_ALG_GCM, key);
  if (!imported.imported()) return false;
  std::size_t written = 0;
  return psa_aead_decrypt(imported.id(), PSA_ALG_GCM, iv.data(), iv.size(), aad.data(), aad.size(), sealed.data(),
                          sealed.size(), out.data(), out.size(), &written) == PSA_SUCCESS &&
         written == sealed.size() - kGcmTagBytes;
}

// esp_random draws on RF noise once the Bluetooth radio is on, which is the only time the
// link needs nonces.
void fillRandom(std::span<std::uint8_t> bytes) { esp_fill_random(bytes.data(), bytes.size()); }

}  // namespace emcool::ble
