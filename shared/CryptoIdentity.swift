import CryptoKit
import Foundation
import Network
import Security

enum CryptoIdentityError: Error, LocalizedError {
    case certificateCreationFailed
    case secKeyCreationFailed(String)
    case secIdentityCreationFailed

    var errorDescription: String? {
        switch self {
        case .certificateCreationFailed:
            return "Failed to construct X.509 DER certificate."
        case .secKeyCreationFailed(let detail):
            return "Failed to create SecKey: \(detail)"
        case .secIdentityCreationFailed:
            return "Failed to bind certificate and private key into SecIdentity."
        }
    }
}

struct PersistedIdentityRecord: Codable, Sendable, Equatable {
    let privateKeyRaw: Data // 32-byte P-256 scalar
    let certificateDER: Data
}

final class CryptoIdentity: @unchecked Sendable {
    let privateKey: P256.Signing.PrivateKey
    let certificateDER: Data
    let certificateSHA256: Data
    let secCertificate: SecCertificate
    let secKey: SecKey
    let secIdentity: SecIdentity

    convenience init(record: PersistedIdentityRecord) throws {
        let priv = try P256.Signing.PrivateKey(rawRepresentation: record.privateKeyRaw)
        try self.init(privateKey: priv, certificateDER: record.certificateDER)
    }

    static func generateNew(commonName: String = "kv2") throws -> CryptoIdentity {
        let priv = P256.Signing.PrivateKey()
        let certDER = try ASN1X509Builder.makeSelfSignedP256Cert(privateKey: priv, commonName: commonName)
        return try CryptoIdentity(privateKey: priv, certificateDER: certDER)
    }

    init(privateKey: P256.Signing.PrivateKey, certificateDER: Data) throws {
        self.privateKey = privateKey
        self.certificateDER = certificateDER
        self.certificateSHA256 = Data(SHA256.hash(data: certificateDER))

        guard let cert = SecCertificateCreateWithData(nil, certificateDER as CFData) else {
            throw CryptoIdentityError.certificateCreationFailed
        }
        self.secCertificate = cert

        let keyAttrs: [CFString: Any] = [
            kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeyClass: kSecAttrKeyClassPrivate,
            kSecAttrKeySizeInBits: 256
        ]
        var unmanagedErr: Unmanaged<CFError>?
        guard let sk = SecKeyCreateWithData(privateKey.x963Representation as CFData, keyAttrs as CFDictionary, &unmanagedErr) else {
            let msg = unmanagedErr?.takeRetainedValue().localizedDescription ?? "unknown"
            throw CryptoIdentityError.secKeyCreationFailed(msg)
        }
        self.secKey = sk

        guard let ident = SecIdentityCreate(nil, cert, sk) else {
            throw CryptoIdentityError.secIdentityCreationFailed
        }
        self.secIdentity = ident
    }

    var persistedRecord: PersistedIdentityRecord {
        PersistedIdentityRecord(privateKeyRaw: privateKey.rawRepresentation, certificateDER: certificateDER)
    }

    var secProtocolIdentity: sec_identity_t? {
        sec_identity_create(secIdentity)
    }

    var fingerprintHex: String {
        certificateSHA256.map { String(format: "%02x", $0) }.joined()
    }

    static func constantTimeEqual(_ a: Data, _ b: Data) -> Bool {
        guard a.count == b.count, !a.isEmpty else { return false }
        var diff: UInt8 = 0
        for i in 0..<a.count {
            diff |= (a[a.startIndex + i] ^ b[b.startIndex + i])
        }
        return diff == 0
    }

    static func computePairingProof(
        secret: Data,
        serverCertSHA256: Data,
        clientCertSHA256: Data,
        role: String
    ) -> Data {
        var salt = Data()
        salt.append(serverCertSHA256)
        salt.append(clientCertSHA256)
        let prk = HKDF<SHA256>.deriveKey(
            inputKeyMaterial: SymmetricKey(data: secret),
            salt: salt,
            info: Data("kv2-pair-v1".utf8),
            outputByteCount: 32
        )
        let mac = HMAC<SHA256>.authenticationCode(for: Data(role.utf8), using: prk)
        return Data(mac)
    }

    /// Creates TLS 1.3 NWParameters with strict certificate pinning and optional mutual authentication.
    static func makeTLSParameters(
        localIdentity: CryptoIdentity,
        requireClientCert: Bool,
        verifyQueue: DispatchQueue,
        validatePeerCertSHA256: @escaping @Sendable (Data, Data) -> Bool // (peerCertSHA256, peerCertDER) -> accepted
    ) -> NWParameters {
        let tlsOptions = NWProtocolTLS.Options()
        let secOpts = tlsOptions.securityProtocolOptions

        sec_protocol_options_set_min_tls_protocol_version(secOpts, .TLSv13)
        sec_protocol_options_set_max_tls_protocol_version(secOpts, .TLSv13)

        if let protoIdent = localIdentity.secProtocolIdentity {
            sec_protocol_options_set_local_identity(secOpts, protoIdent)
        }

        if requireClientCert {
            sec_protocol_options_set_peer_authentication_required(secOpts, true)
        }

        sec_protocol_options_set_verify_block(secOpts, { _, secTrust, completion in
            let trust = sec_trust_copy_ref(secTrust).takeRetainedValue()
            guard let chain = SecTrustCopyCertificateChain(trust),
                  CFArrayGetCount(chain) > 0 else {
                completion(false)
                return
            }
            let leaf = unsafeBitCast(CFArrayGetValueAtIndex(chain, 0), to: SecCertificate.self)
            let der = SecCertificateCopyData(leaf) as Data
            let hash = Data(SHA256.hash(data: der))
            completion(validatePeerCertSHA256(hash, der))
        }, verifyQueue)

        let tcpOptions = NWProtocolTCP.Options()
        tcpOptions.noDelay = true
        tcpOptions.enableKeepalive = true
        tcpOptions.keepaliveIdle = 2
        tcpOptions.keepaliveInterval = 1
        tcpOptions.keepaliveCount = 3
        tcpOptions.connectionDropTime = 5

        let params = NWParameters(tls: tlsOptions, tcp: tcpOptions)
        params.includePeerToPeer = false
        return params
    }
}

// MARK: - Minimal Deterministic ASN.1 DER X.509 v3 Builder

private enum ASN1X509Builder {
    static func encodeLength(_ len: Int) -> Data {
        if len < 0x80 {
            return Data([UInt8(len)])
        } else if len <= 0xFF {
            return Data([0x81, UInt8(len)])
        } else {
            return Data([0x82, UInt8((len >> 8) & 0xFF), UInt8(len & 0xFF)])
        }
    }

    static func tlv(_ tag: UInt8, _ content: Data) -> Data {
        var out = Data([tag])
        out.append(encodeLength(content.count))
        out.append(content)
        return out
    }

    static func sequence(_ items: [Data]) -> Data {
        var body = Data()
        for item in items { body.append(item) }
        return tlv(0x30, body)
    }

    static func set(_ items: [Data]) -> Data {
        var body = Data()
        for item in items { body.append(item) }
        return tlv(0x31, body)
    }

    static func oid(_ bytes: [UInt8]) -> Data {
        tlv(0x06, Data(bytes))
    }

    static func utf8String(_ s: String) -> Data {
        tlv(0x0C, Data(s.utf8))
    }

    static func utcTime(_ s: String) -> Data {
        tlv(0x17, Data(s.utf8))
    }

    static func octetString(_ d: Data) -> Data {
        tlv(0x04, d)
    }

    static func bitString(_ d: Data, unusedBits: UInt8 = 0) -> Data {
        var body = Data([unusedBits])
        body.append(d)
        return tlv(0x03, body)
    }

    static func booleanTrue() -> Data {
        tlv(0x01, Data([0xFF]))
    }

    static func makeSelfSignedP256Cert(privateKey: P256.Signing.PrivateKey, commonName: String) throws -> Data {
        // Version v3: [0] EXPLICIT INTEGER 2
        let version = tlv(0xA0, tlv(0x02, Data([0x02])))

        // 16-byte positive random serial number
        var serialBytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, serialBytes.count, &serialBytes)
        serialBytes[0] = (serialBytes[0] & 0x7F) | 0x01
        let serial = tlv(0x02, Data(serialBytes))

        // ecdsa-with-SHA256 (1.2.840.10045.4.3.2)
        let sigAlg = sequence([oid([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02])])

        // Name: CN=<commonName> (OID 2.5.4.3 = 55 04 03)
        let rdn = set([sequence([oid([0x55, 0x04, 0x03]), utf8String(commonName)])])
        let name = sequence([rdn])

        // Validity: 2025-01-01 to 2036-01-01 UTC
        let validity = sequence([
            utcTime("250101000000Z"),
            utcTime("360101000000Z")
        ])

        // SubjectPublicKeyInfo directly from CryptoKit
        let spki = privateKey.publicKey.derRepresentation

        // X.509 v3 Extensions:
        // 1. BasicConstraints (2.5.29.19 = 55 1D 13), critical, CA:FALSE
        let extBasicConstraints = sequence([
            oid([0x55, 0x1D, 0x13]),
            booleanTrue(),
            octetString(sequence([]))
        ])

        // 2. KeyUsage (2.5.29.15 = 55 1D 0F), critical, digitalSignature (bit 0)
        let extKeyUsage = sequence([
            oid([0x55, 0x1D, 0x0F]),
            booleanTrue(),
            octetString(bitString(Data([0x80]), unusedBits: 7))
        ])

        // 3. ExtendedKeyUsage (2.5.29.37 = 55 1D 25), serverAuth + clientAuth
        let extEKU = sequence([
            oid([0x55, 0x1D, 0x25]),
            octetString(sequence([
                oid([0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01]), // serverAuth
                oid([0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02])  // clientAuth
            ]))
        ])

        let extensions = tlv(0xA3, sequence([extBasicConstraints, extKeyUsage, extEKU]))

        let tbsCert = sequence([
            version,
            serial,
            sigAlg,
            name,
            validity,
            name,
            spki,
            extensions
        ])

        let signature = try privateKey.signature(for: tbsCert)
        return sequence([
            tbsCert,
            sigAlg,
            bitString(signature.derRepresentation, unusedBits: 0)
        ])
    }
}
