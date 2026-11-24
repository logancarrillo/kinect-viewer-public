// C++ (OpenSSL 3) port of shared/CryptoIdentity.swift: self-signed ECDSA P-256 identity built with the
// same deterministic ASN.1 layout as ASN1X509Builder, SHA-256 fingerprints, constant-time compare and
// the HKDF-SHA256 + HMAC-SHA256 pairing proof.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace kvhost {

using Bytes = std::vector<uint8_t>;

Bytes sha256(const uint8_t* data, size_t n);
inline Bytes sha256(const Bytes& b) { return sha256(b.data(), b.size()); }
Bytes hmacSha256(const Bytes& key, const uint8_t* data, size_t n);
bool hkdfSha256(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t out_len, Bytes& out);
bool randomBytes(uint8_t* out, size_t n);

// Swift semantics: false when the lengths differ or either side is empty.
bool constantTimeEqual(const Bytes& a, const Bytes& b);

// HKDF<SHA256>(ikm: secret, salt: serverFP || clientFP, info: "kv2-pair-v1", 32) then HMAC<SHA256>(role).
Bytes computePairingProof(const Bytes& secret, const Bytes& serverCertSHA256, const Bytes& clientCertSHA256,
                          const std::string& role);

struct PkeyDeleter {
    void operator()(EVP_PKEY* k) const;
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

PkeyPtr generateP256Key();
PkeyPtr p256KeyFromScalar(const Bytes& raw32);
bool p256ScalarOf(EVP_PKEY* key, Bytes& raw32);
Bytes subjectPublicKeyInfoDer(EVP_PKEY* key); // == CryptoKit publicKey.derRepresentation (91 bytes)

// ASN1X509Builder: TBS for the given key; serial16 is the raw random serial before Swift's fix-up.
Bytes buildTbsCertificate(EVP_PKEY* key, const Bytes& serial16, const std::string& common_name);
// Full self-signed certificate (ECDSA-SHA256 over the TBS). Random serial when serial16 is empty.
Bytes makeSelfSignedP256Cert(EVP_PKEY* key, const std::string& common_name, const Bytes& serial16 = {});

class CryptoIdentity {
public:
    static std::unique_ptr<CryptoIdentity> generate(const std::string& common_name, std::string* err = nullptr);
    // Rebuilds from the persisted record; fails if the key does not match the certificate.
    static std::unique_ptr<CryptoIdentity> fromRecord(const Bytes& private_key_raw, const Bytes& certificate_der,
                                                      std::string* err = nullptr);

    EVP_PKEY* key() const { return key_.get(); }
    const Bytes& privateKeyRaw() const { return raw_; }
    const Bytes& certificateDER() const { return der_; }
    const Bytes& certificateSHA256() const { return fp_; }
    std::string fingerprintHex() const;

private:
    PkeyPtr key_;
    Bytes raw_;
    Bytes der_;
    Bytes fp_;
};

std::string hexLower(const uint8_t* d, size_t n);

} // namespace kvhost
