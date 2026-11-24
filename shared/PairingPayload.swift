import Foundation
import Security

enum BonjourConstants {
    /// Opaque mDNS service type for live mutually-authenticated stream
    static let streamServiceType = "_kv2s._tcp"
    /// Opaque mDNS service type advertised ONLY during active 120s pairing window
    static let pairingServiceType = "_kv2p._tcp"
    static let domain = "local."
}

struct QRPairingPayload: Equatable, Sendable {
    let serviceId: String
    let serverCertSHA256: Data
    let oneTimeToken: Data
    let hostIP: String
    let pairPort: UInt16

    func toURIString() -> String {
        var comps = URLComponents()
        comps.scheme = "kv2"
        comps.host = "pair"
        comps.queryItems = [
            URLQueryItem(name: "s", value: serviceId),
            URLQueryItem(name: "fp", value: Self.base64URL(serverCertSHA256)),
            URLQueryItem(name: "tok", value: Self.base64URL(oneTimeToken)),
            URLQueryItem(name: "h", value: hostIP),
            URLQueryItem(name: "p", value: String(pairPort))
        ]
        return comps.string ?? ""
    }

    static func parse(_ uriString: String) -> QRPairingPayload? {
        guard let comps = URLComponents(string: uriString.trimmingCharacters(in: .whitespacesAndNewlines)),
              comps.scheme == "kv2",
              comps.host == "pair",
              let items = comps.queryItems else {
            return nil
        }
        var dict: [String: String] = [:]
        for item in items {
            if let v = item.value { dict[item.name] = v }
        }
        guard let s = dict["s"], !s.isEmpty,
              let fpStr = dict["fp"], let fp = fromBase64URL(fpStr), fp.count == 32,
              let tokStr = dict["tok"], let tok = fromBase64URL(tokStr), tok.count >= 16,
              let h = dict["h"], !h.isEmpty,
              let pStr = dict["p"], let p = UInt16(pStr), p > 0 else {
            return nil
        }
        return QRPairingPayload(serviceId: s, serverCertSHA256: fp, oneTimeToken: tok, hostIP: h, pairPort: p)
    }

    static func normalizeShortCode(_ code: String) -> String {
        code.filter { $0.isNumber }
    }

    static func generateShortCode() -> (formatted: String, normalized: String) {
        var bytes = [UInt8](repeating: 0, count: 4)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        let num = bytes.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) } % 100_000_000
        let digits = String(format: "%08u", num)
        let idx = digits.index(digits.startIndex, offsetBy: 4)
        return ("\(digits[..<idx])-\(digits[idx...])", digits)
    }

    static func base64URL(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    static func fromBase64URL(_ str: String) -> Data? {
        var s = str
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        let rem = s.count % 4
        if rem > 0 {
            s.append(String(repeating: "=", count: 4 - rem))
        }
        return Data(base64Encoded: s)
    }
}

/// Protocol v3 outside-the-home one-time invite URI (`docs/protocol-v3-pet-camera.md` §8.4).
/// `kv2://invite?v=3&s=<serviceId>&fp=<b64url(32B)>&tok=<b64url(16B)>&h=<host>&p=<port>&lh=<lanHost>&exp=<unixSec>&r=<owner|viewer>`
struct InviteLinkPayload: Equatable, Sendable {
    static let clockSlackSeconds: UInt64 = 60

    let version: Int
    let serviceId: String
    let serverCertSHA256: Data
    let oneTimeToken: Data
    let outsideHost: String
    let outsidePort: UInt16
    let lanHost: String
    let expiresUnixSec: UInt64
    let roleHint: PetRole

    init(
        version: Int = 3,
        serviceId: String,
        serverCertSHA256: Data,
        oneTimeToken: Data,
        outsideHost: String,
        outsidePort: UInt16,
        lanHost: String,
        expiresUnixSec: UInt64,
        roleHint: PetRole
    ) {
        self.version = version
        self.serviceId = serviceId
        self.serverCertSHA256 = serverCertSHA256
        self.oneTimeToken = oneTimeToken
        self.outsideHost = outsideHost
        self.outsidePort = outsidePort
        self.lanHost = lanHost
        self.expiresUnixSec = expiresUnixSec
        self.roleHint = roleHint
    }

    func isExpired(nowUnixSec: UInt64 = UInt64(Date().timeIntervalSince1970)) -> Bool {
        nowUnixSec > expiresUnixSec &+ Self.clockSlackSeconds
    }

    func toURIString() -> String {
        var comps = URLComponents()
        comps.scheme = "kv2"
        comps.host = "invite"
        comps.queryItems = [
            URLQueryItem(name: "v", value: String(version)),
            URLQueryItem(name: "s", value: serviceId),
            URLQueryItem(name: "fp", value: QRPairingPayload.base64URL(serverCertSHA256)),
            URLQueryItem(name: "tok", value: QRPairingPayload.base64URL(oneTimeToken)),
            URLQueryItem(name: "h", value: outsideHost),
            URLQueryItem(name: "p", value: String(outsidePort)),
            URLQueryItem(name: "lh", value: lanHost),
            URLQueryItem(name: "exp", value: String(expiresUnixSec)),
            URLQueryItem(name: "r", value: roleHint.rawValue)
        ]
        return comps.string ?? ""
    }

    static func parse(_ uriString: String) -> InviteLinkPayload? {
        let trimmed = uriString.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let comps = URLComponents(string: trimmed),
              comps.scheme == "kv2",
              comps.host == "invite",
              let items = comps.queryItems else {
            return nil
        }
        var dict: [String: String] = [:]
        for item in items {
            if let v = item.value { dict[item.name] = v }
        }
        guard dict["v"] == "3",
              let s = dict["s"], !s.isEmpty,
              let fpStr = dict["fp"], let fp = QRPairingPayload.fromBase64URL(fpStr), fp.count == 32,
              let tokStr = dict["tok"], let tok = QRPairingPayload.fromBase64URL(tokStr), tok.count == 16,
              let h = dict["h"], !h.isEmpty,
              let pStr = dict["p"], let p = UInt16(pStr), p > 0,
              let lh = dict["lh"], !lh.isEmpty,
              let expStr = dict["exp"], let exp = UInt64(expStr), exp > 0,
              let rStr = dict["r"], (rStr == "owner" || rStr == "viewer"),
              let role = PetRole(rawValue: rStr) else {
            return nil
        }
        return InviteLinkPayload(
            version: 3,
            serviceId: s,
            serverCertSHA256: fp,
            oneTimeToken: tok,
            outsideHost: h,
            outsidePort: p,
            lanHost: lh,
            expiresUnixSec: exp,
            roleHint: role
        )
    }
}

/// Protocol v3 outside endpoint update link (`docs/protocol-v3-pet-camera.md` §8.4).
/// `kv2://endpoint?v=3&s=<serviceId>&fp=<b64url(32B)>&h=<host>&p=<port>`
struct EndpointLinkPayload: Equatable, Sendable {
    let version: Int
    let serviceId: String
    let serverCertSHA256: Data
    let outsideHost: String
    let outsidePort: UInt16

    init(
        version: Int = 3,
        serviceId: String,
        serverCertSHA256: Data,
        outsideHost: String,
        outsidePort: UInt16
    ) {
        self.version = version
        self.serviceId = serviceId
        self.serverCertSHA256 = serverCertSHA256
        self.outsideHost = outsideHost
        self.outsidePort = outsidePort
    }

    func toURIString() -> String {
        var comps = URLComponents()
        comps.scheme = "kv2"
        comps.host = "endpoint"
        comps.queryItems = [
            URLQueryItem(name: "v", value: String(version)),
            URLQueryItem(name: "s", value: serviceId),
            URLQueryItem(name: "fp", value: QRPairingPayload.base64URL(serverCertSHA256)),
            URLQueryItem(name: "h", value: outsideHost),
            URLQueryItem(name: "p", value: String(outsidePort))
        ]
        return comps.string ?? ""
    }

    static func parse(_ uriString: String) -> EndpointLinkPayload? {
        let trimmed = uriString.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let comps = URLComponents(string: trimmed),
              comps.scheme == "kv2",
              comps.host == "endpoint",
              let items = comps.queryItems else {
            return nil
        }
        var dict: [String: String] = [:]
        for item in items {
            if let v = item.value { dict[item.name] = v }
        }
        guard dict["v"] == "3",
              let s = dict["s"], !s.isEmpty,
              let fpStr = dict["fp"], let fp = QRPairingPayload.fromBase64URL(fpStr), fp.count == 32,
              let h = dict["h"], !h.isEmpty,
              let pStr = dict["p"], let p = UInt16(pStr), p > 0 else {
            return nil
        }
        return EndpointLinkPayload(
            version: 3,
            serviceId: s,
            serverCertSHA256: fp,
            outsideHost: h,
            outsidePort: p
        )
    }
}

enum ParsedCameraLink: Equatable, Sendable {
    case lanPair(QRPairingPayload)
    case invite(InviteLinkPayload)
    case endpoint(EndpointLinkPayload)

    static func isSupportedQRURI(_ rawString: String) -> Bool {
        let trimmed = rawString.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.hasPrefix("kv2://pair") ||
               trimmed.hasPrefix("kv2://invite") ||
               trimmed.hasPrefix("kv2://endpoint")
    }

    static func parse(_ rawString: String) -> ParsedCameraLink? {
        let trimmed = rawString.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("kv2://pair"), let qr = QRPairingPayload.parse(trimmed) {
            return .lanPair(qr)
        }
        if trimmed.hasPrefix("kv2://invite"), let inv = InviteLinkPayload.parse(trimmed) {
            return .invite(inv)
        }
        if trimmed.hasPrefix("kv2://endpoint"), let ep = EndpointLinkPayload.parse(trimmed) {
            return .endpoint(ep)
        }
        return nil
    }
}

struct ClientPairingState: Codable, Sendable, Equatable {
    let clientIdentity: PersistedIdentityRecord
    var serviceId: String?
    var pinnedServerCertSHA256: Data?
    var lastKnownHost: String?
    var lastKnownStreamPort: UInt16?
    var pairedDeviceId: String? = nil
    var pairedRole: PetRole? = nil
    var lastKnownHostProtocolVersion: Int? = nil
    var outsideEndpoint: PetEndpoint? = nil
    /// Extra outside addresses from 0x0B `outsideCandidates` (absent in older stores, which still load).
    var outsideCandidates: [PetEndpoint]? = nil
}

enum ClientPairingLoadResult: Equatable, Sendable {
    case loaded(ClientPairingState)
    case notFound
    case accessDeniedOrLocked(OSStatus)
    case corruptedData
}

enum ClientPairingStoreError: LocalizedError, Equatable, Sendable {
    case keychainAccessRequired(deviceKind: String, status: OSStatus)
    case corruptedKeychainPairingState
    case keychainSaveFailed

    var errorDescription: String? {
        switch self {
        case .keychainAccessRequired(let kind, _):
            return PetCameraUserStrings.keychainAccessRequiredText(forDeviceKind: kind)
        case .corruptedKeychainPairingState:
            return "The saved camera pairing in the Keychain is damaged."
        case .keychainSaveFailed:
            return "Couldn't save this device's pairing key to the Keychain."
        }
    }
}

struct ClientKeychainPrimitives: Sendable {
    var copyMatching: @Sendable (CFDictionary, UnsafeMutablePointer<CFTypeRef?>?) -> OSStatus
    var update: @Sendable (CFDictionary, CFDictionary) -> OSStatus
    var add: @Sendable (CFDictionary, UnsafeMutablePointer<CFTypeRef?>?) -> OSStatus
    var delete: @Sendable (CFDictionary) -> OSStatus

    static let system = ClientKeychainPrimitives(
        copyMatching: { query, result in SecItemCopyMatching(query, result) },
        update: { query, attrs in SecItemUpdate(query, attrs) },
        add: { query, result in SecItemAdd(query, result) },
        delete: { query in SecItemDelete(query) }
    )
}

/// Keychain storage for client pairing keys (ordinary Keychain items, not hardware-backed; zero filesystem writes).
/// Keychain safety rules:
/// - Distinguishes `errSecItemNotFound` from denied/locked (`errSecAuthFailed`, `errSecInteractionNotAllowed`, etc.) and corrupted bytes;
/// - Updates first via `SecItemUpdate` and NEVER calls `SecItemDelete` on an error;
/// - Inserts via `SecItemAdd` ONLY when `SecItemUpdate` reports `errSecItemNotFound`.
enum KeychainPairingStore {
    // Under the build's own prefix (KeychainServicePrefix), so an install keeps the keys it paired with.
    static let defaultService = KeychainServicePrefix.current + ".pocket.pairing"
    static let macViewerService = KeychainServicePrefix.current + ".mac.viewer.pairing"
    static let defaultAccount = "v1"

    static func loadResult(
        service: String = defaultService,
        account: String = defaultAccount,
        primitives: ClientKeychainPrimitives = .system
    ) -> ClientPairingLoadResult {
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account,
            kSecReturnData: true,
            kSecMatchLimit: kSecMatchLimitOne
        ]
        var result: CFTypeRef?
        let status = primitives.copyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound {
            return .notFound
        }
        guard status == errSecSuccess else {
            return .accessDeniedOrLocked(status)
        }
        guard let data = result as? Data,
              let decoded = try? JSONDecoder().decode(ClientPairingState.self, from: data) else {
            return .corruptedData
        }
        return .loaded(decoded)
    }

    @discardableResult
    static func save(
        _ state: ClientPairingState,
        service: String = defaultService,
        account: String = defaultAccount,
        primitives: ClientKeychainPrimitives = .system
    ) -> Bool {
        guard let data = try? JSONEncoder().encode(state) else { return false }
        let baseQuery: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account
        ]
        let updateAttrs: [CFString: Any] = [
            kSecValueData: data
        ]
        let updateStatus = primitives.update(baseQuery as CFDictionary, updateAttrs as CFDictionary)
        if updateStatus == errSecSuccess {
            return true
        }
        // Never delete on an update error other than errSecItemNotFound, or a denied/locked Keychain would destroy the paired identity.
        guard updateStatus == errSecItemNotFound else {
            return false
        }

        var addQuery = baseQuery
        addQuery[kSecValueData] = data
        addQuery[kSecAttrAccessible] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        let addStatus = primitives.add(addQuery as CFDictionary, nil)
        return addStatus == errSecSuccess
    }

    static func wipe(
        service: String = defaultService,
        account: String = defaultAccount,
        primitives: ClientKeychainPrimitives = .system
    ) {
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account
        ]
        _ = primitives.delete(query as CFDictionary)
    }
}
