// PLUG-4: the identity `pairing.revoke` matches on.
//
// A revoke deletes an authorized certificate, so the only thing standing
// between "drop this client" and "drop somebody else's client" is that the
// digest recomputed from the stored PEM is the same digest the pairing attempt
// published. This builds a real certificate, takes its DER SHA-256 with the
// plain EVP API, and requires managed_revocation to produce exactly that.
//
//   g++ -std=c++20 -I src tests/res01/revocation_fingerprint_test.cpp -lcrypto -o /tmp/revocation_test
#include "managed_revocation.h"

#include <cassert>
#include <iostream>
#include <string>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {
  std::string hex_sha256(const unsigned char *data, int size) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    assert(context != nullptr);
    assert(EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1);
    assert(EVP_DigestUpdate(context, data, static_cast<std::size_t>(size)) == 1);
    assert(EVP_DigestFinal_ex(context, digest, &length) == 1);
    EVP_MD_CTX_free(context);
    assert(length == 32);
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) {
      result += HEX[digest[i] >> 4];
      result += HEX[digest[i] & 15];
    }
    return result;
  }

  X509 *self_signed(const char *common_name) {
    EVP_PKEY *key = EVP_RSA_gen(2048);
    assert(key != nullptr);
    X509 *certificate = X509_new();
    assert(certificate != nullptr);
    assert(X509_set_version(certificate, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
    X509_gmtime_adj(X509_get_notBefore(certificate), 0);
    X509_gmtime_adj(X509_get_notAfter(certificate), 60 * 60 * 24);
    assert(X509_set_pubkey(certificate, key) == 1);
    X509_NAME *name = X509_get_subject_name(certificate);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char *>(common_name), -1, -1, 0) == 1);
    assert(X509_set_issuer_name(certificate, name) == 1);
    assert(X509_sign(certificate, key, EVP_sha256()) > 0);
    EVP_PKEY_free(key);
    return certificate;
  }

  std::string to_pem(X509 *certificate) {
    BIO *sink = BIO_new(BIO_s_mem());
    assert(sink != nullptr);
    assert(PEM_write_bio_X509(sink, certificate) == 1);
    char *data = nullptr;
    const long size = BIO_get_mem_data(sink, &data);
    std::string pem(data, static_cast<std::size_t>(size));
    BIO_free(sink);
    return pem;
  }
}

int main() {
  X509 *certificate = self_signed("omodachi-revoke-fixture");
  unsigned char *der = nullptr;
  const int der_size = i2d_X509(certificate, &der);
  assert(der_size > 0);
  const std::string expected = hex_sha256(der, der_size);
  OPENSSL_free(der);

  const std::string pem = to_pem(certificate);
  // The stored PEM and the live certificate name the same client.
  assert(managed_revocation::fingerprint(certificate) == expected);
  assert(managed_revocation::fingerprint_of_pem(pem) == expected);
  assert(managed_revocation::is_fingerprint(expected));

  // A second certificate is a different client, even with the same subject.
  X509 *other = self_signed("omodachi-revoke-fixture");
  const std::string other_pem = to_pem(other);
  assert(managed_revocation::fingerprint_of_pem(other_pem) != expected);
  X509_free(other);
  X509_free(certificate);

  // Nothing unparsable, empty or differently spelled may ever compare equal to
  // a real fingerprint: an empty result is what a corrupt record produces.
  assert(managed_revocation::fingerprint_of_pem("").empty());
  assert(managed_revocation::fingerprint_of_pem("-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n").empty());
  assert(managed_revocation::fingerprint(nullptr).empty());
  assert(!managed_revocation::is_fingerprint(""));
  assert(!managed_revocation::is_fingerprint(std::string(63, 'a')));
  assert(!managed_revocation::is_fingerprint(std::string(65, 'a')));
  assert(!managed_revocation::is_fingerprint(std::string(64, 'A')));
  assert(!managed_revocation::is_fingerprint(std::string(64, 'g')));
  assert(!managed_revocation::is_fingerprint(std::string(63, 'a') + " "));

  std::cout << "revocation_fingerprint: PASS\n";
}
