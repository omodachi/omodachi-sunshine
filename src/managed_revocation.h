/**
 * @file src/managed_revocation.h
 * @brief The certificate identity a persistent revoke matches on.
 *
 * The managed pairing attempt names a client by the lowercase hex SHA-256 of
 * its canonical DER leaf certificate (`nvhttp::pair`, `X509_digest`). The
 * persistent client store keeps the same certificate as PEM, so a revoke has
 * to recompute that exact digest from the stored PEM and compare it to the
 * fingerprint the caller supplied. Nothing else here is an identity: never
 * match on a name, a uniqueid or a UUID, which the client chooses.
 */
#pragma once
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace managed_revocation {
  inline bool is_fingerprint(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char c : value) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
  }

  /// Lowercase hex SHA-256 of the DER encoding, or an empty string.
  inline std::string fingerprint(X509 *certificate) {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (certificate == nullptr || X509_digest(certificate, EVP_sha256(), bytes, &size) != 1 || size != 32) {
      return {};
    }
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int i = 0; i < size; ++i) {
      result += HEX[bytes[i] >> 4];
      result += HEX[bytes[i] & 15];
    }
    return result;
  }

  /// The same digest for a stored PEM leaf. An unparsable record yields an
  /// empty string, which never equals a caller-supplied fingerprint.
  inline std::string fingerprint_of_pem(const std::string &pem) {
    if (pem.empty() || pem.size() > 1024 * 1024) return {};
    BIO *source = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (source == nullptr) return {};
    X509 *certificate = PEM_read_bio_X509(source, nullptr, nullptr, nullptr);
    BIO_free(source);
    if (certificate == nullptr) return {};
    std::string result = fingerprint(certificate);
    X509_free(certificate);
    return result;
  }
}
