#include "crypto_identity.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <cstring>

namespace kvhost {

void PkeyDeleter::operator()(EVP_PKEY* k) const {
    EVP_PKEY_free(k);
}

Bytes sha256(const uint8_t* data, size_t n) {
    Bytes out(32);
    unsigned int len = 0;
    EVP_Digest(data, n, out.data(), &len, EVP_sha256(), nullptr);
    out.resize(len);
    return out;
}

Bytes hmacSha256(const Bytes& key, const uint8_t* data, size_t n) {
    Bytes out(32);
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data, n, out.data(), &len);
    out.resize(len);
    return out;
}

bool hkdfSha256(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t out_len, Bytes& out) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return false;
    out.assign(out_len, 0);
    size_t len = out_len;
    const bool ok = EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
                    EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt.empty() ? nullptr : salt.data(), static_cast<int>(salt.size())) > 0 &&
                    EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm.data(), static_cast<int>(ikm.size())) > 0 &&
                    EVP_PKEY_CTX_add1_hkdf_info(ctx, info.data(), static_cast<int>(info.size())) > 0 &&
                    EVP_PKEY_derive(ctx, out.data(), &len) > 0 && len == out_len;
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

bool randomBytes(uint8_t* out, size_t n) {
    return RAND_bytes(out, static_cast<int>(n)) == 1;
}

bool constantTimeEqual(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size() || a.empty()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

Bytes computePairingProof(const Bytes& secret, const Bytes& serverFp, const Bytes& clientFp, const std::string& role) {
    Bytes salt = serverFp;
    salt.insert(salt.end(), clientFp.begin(), clientFp.end());
    const std::string info = "kv2-pair-v1";
    Bytes prk;
    if (!hkdfSha256(secret, salt, Bytes(info.begin(), info.end()), 32, prk)) return {};
    return hmacSha256(prk, reinterpret_cast<const uint8_t*>(role.data()), role.size());
}

std::string hexLower(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += k[d[i] >> 4];
        s += k[d[i] & 15];
    }
    return s;
}

// ---- keys ---------------------------------------------------------------------------------------

PkeyPtr generateP256Key() {
    return PkeyPtr(EVP_EC_gen("prime256v1"));
}

PkeyPtr p256KeyFromScalar(const Bytes& raw32) {
    if (raw32.size() != 32) return nullptr;
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM* priv = BN_bin2bn(raw32.data(), 32, nullptr);
    EC_POINT* pub = group ? EC_POINT_new(group) : nullptr;
    PkeyPtr result;
    unsigned char pub_oct[65];
    size_t pub_len = 0;
    if (group && priv && pub && !BN_is_zero(priv) && BN_cmp(priv, EC_GROUP_get0_order(group)) < 0 &&
        EC_POINT_mul(group, pub, priv, nullptr, nullptr, nullptr) == 1) {
        pub_len = EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED, pub_oct, sizeof(pub_oct), nullptr);
    }
    if (pub_len == 65) {
        OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv);
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub_oct, pub_len);
        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        EVP_PKEY* pkey = nullptr;
        if (ctx && params && EVP_PKEY_fromdata_init(ctx) > 0 && EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params) > 0) {
            result.reset(pkey);
        }
        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
        OSSL_PARAM_BLD_free(bld);
    }
    EC_POINT_free(pub);
    BN_clear_free(priv);
    EC_GROUP_free(group);
    return result;
}

bool p256ScalarOf(EVP_PKEY* key, Bytes& raw32) {
    BIGNUM* bn = nullptr;
    if (!key || EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &bn) != 1) return false;
    raw32.assign(32, 0);
    const bool ok = BN_bn2binpad(bn, raw32.data(), 32) == 32;
    BN_clear_free(bn);
    return ok;
}

Bytes subjectPublicKeyInfoDer(EVP_PKEY* key) {
    unsigned char* der = nullptr;
    const int n = i2d_PUBKEY(key, &der);
    if (n <= 0) return {};
    Bytes out(der, der + n);
    OPENSSL_free(der);
    return out;
}

// ---- ASN1X509Builder ------------------------------------------------------------------------------

namespace {

Bytes encodeLength(size_t len) {
    if (len < 0x80) return {static_cast<uint8_t>(len)};
    if (len <= 0xFF) return {0x81, static_cast<uint8_t>(len)};
    return {0x82, static_cast<uint8_t>((len >> 8) & 0xFF), static_cast<uint8_t>(len & 0xFF)};
}

Bytes tlv(uint8_t tag, const Bytes& content) {
    Bytes out{tag};
    const Bytes l = encodeLength(content.size());
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes cat(std::initializer_list<Bytes> items) {
    Bytes out;
    for (const auto& i : items) out.insert(out.end(), i.begin(), i.end());
    return out;
}

Bytes sequence(std::initializer_list<Bytes> items) { return tlv(0x30, cat(items)); }
Bytes setOf(std::initializer_list<Bytes> items) { return tlv(0x31, cat(items)); }
Bytes oid(std::initializer_list<uint8_t> b) { return tlv(0x06, Bytes(b)); }
Bytes utf8String(const std::string& s) { return tlv(0x0C, Bytes(s.begin(), s.end())); }
Bytes utcTime(const std::string& s) { return tlv(0x17, Bytes(s.begin(), s.end())); }
Bytes octetString(const Bytes& d) { return tlv(0x04, d); }
Bytes bitString(const Bytes& d, uint8_t unused = 0) {
    Bytes body{unused};
    body.insert(body.end(), d.begin(), d.end());
    return tlv(0x03, body);
}
Bytes booleanTrue() { return tlv(0x01, Bytes{0xFF}); }

Bytes ecdsaSigAlg() { return sequence({oid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02})}); }

} // namespace

Bytes buildTbsCertificate(EVP_PKEY* key, const Bytes& serial16, const std::string& cn) {
    if (serial16.size() != 16) return {};
    const Bytes version = tlv(0xA0, tlv(0x02, Bytes{0x02}));
    Bytes serial_bytes = serial16;
    serial_bytes[0] = static_cast<uint8_t>((serial_bytes[0] & 0x7F) | 0x01);
    const Bytes serial = tlv(0x02, serial_bytes);
    const Bytes rdn = setOf({sequence({oid({0x55, 0x04, 0x03}), utf8String(cn)})});
    const Bytes name = sequence({rdn});
    const Bytes validity = sequence({utcTime("250101000000Z"), utcTime("360101000000Z")});
    const Bytes spki = subjectPublicKeyInfoDer(key);
    if (spki.empty()) return {};
    const Bytes ext_bc = sequence({oid({0x55, 0x1D, 0x13}), booleanTrue(), octetString(sequence({}))});
    const Bytes ext_ku = sequence({oid({0x55, 0x1D, 0x0F}), booleanTrue(), octetString(bitString(Bytes{0x80}, 7))});
    const Bytes ext_eku = sequence({oid({0x55, 0x1D, 0x25}),
                                    octetString(sequence({oid({0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01}),
                                                          oid({0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02})}))});
    const Bytes extensions = tlv(0xA3, sequence({ext_bc, ext_ku, ext_eku}));
    return sequence({version, serial, ecdsaSigAlg(), name, validity, name, spki, extensions});
}

Bytes makeSelfSignedP256Cert(EVP_PKEY* key, const std::string& cn, const Bytes& serial16_in) {
    Bytes serial16 = serial16_in;
    if (serial16.empty()) {
        serial16.assign(16, 0);
        if (!randomBytes(serial16.data(), serial16.size())) return {};
    }
    const Bytes tbs = buildTbsCertificate(key, serial16, cn);
    if (tbs.empty()) return {};

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    Bytes sig;
    size_t sig_len = 0;
    if (md && EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSign(md, nullptr, &sig_len, tbs.data(), tbs.size()) == 1) {
        sig.resize(sig_len);
        if (EVP_DigestSign(md, sig.data(), &sig_len, tbs.data(), tbs.size()) == 1) {
            sig.resize(sig_len); // DER ECDSA-Sig-Value, same as CryptoKit's derRepresentation
        } else {
            sig.clear();
        }
    }
    EVP_MD_CTX_free(md);
    if (sig.empty()) return {};
    return sequence({tbs, ecdsaSigAlg(), bitString(sig, 0)});
}

// ---- CryptoIdentity -------------------------------------------------------------------------------

std::unique_ptr<CryptoIdentity> CryptoIdentity::generate(const std::string& cn, std::string* err) {
    auto id = std::unique_ptr<CryptoIdentity>(new CryptoIdentity());
    id->key_ = generateP256Key();
    if (!id->key_ || !p256ScalarOf(id->key_.get(), id->raw_)) {
        if (err) *err = "P-256 key generation failed";
        return nullptr;
    }
    id->der_ = makeSelfSignedP256Cert(id->key_.get(), cn);
    if (id->der_.empty()) {
        if (err) *err = "certificate construction failed";
        return nullptr;
    }
    id->fp_ = sha256(id->der_);
    return id;
}

std::unique_ptr<CryptoIdentity> CryptoIdentity::fromRecord(const Bytes& raw, const Bytes& der, std::string* err) {
    auto id = std::unique_ptr<CryptoIdentity>(new CryptoIdentity());
    id->key_ = p256KeyFromScalar(raw);
    if (!id->key_) {
        if (err) *err = "invalid P-256 private key";
        return nullptr;
    }
    const unsigned char* p = der.data();
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (!cert || p != der.data() + der.size()) {
        X509_free(cert);
        if (err) *err = "invalid certificate DER";
        return nullptr;
    }
    const bool match = X509_check_private_key(cert, id->key_.get()) == 1;
    X509_free(cert);
    if (!match) {
        if (err) *err = "certificate does not match private key";
        return nullptr;
    }
    id->raw_ = raw;
    id->der_ = der;
    id->fp_ = sha256(der);
    return id;
}

std::string CryptoIdentity::fingerprintHex() const {
    return hexLower(fp_.data(), fp_.size());
}

} // namespace kvhost
