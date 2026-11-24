import CoreImage
import CryptoKit
import Darwin
import Foundation
import Network
import Security

enum HostIdentityLoadResult: Sendable {
    case loaded(PersistedIdentityRecord)
    case notFound
    case accessDeniedOrLocked(OSStatus)
    case corruptedData
}

enum HostPairingKeyError: LocalizedError, Equatable, Sendable {
    case keychainAccessRequired(OSStatus)
    case missingKeychainIdentityForExistingPairing
    case mismatchedKeychainIdentity
    case corruptedKeychainIdentity
    case corruptedPairingStateFile
    case keychainSaveFailed

    static let keychainAccessPromptMessage = "Allow Keychain access to keep your iPhone paired."
    static let keychainSaveFailedMessage = "Couldn't save this Mac's pairing key to the Keychain."
    static let missingKeychainIdentityMessage = "This Mac's pairing key is missing from the Keychain."
    static let mismatchedKeychainIdentityMessage = "This Mac's pairing key doesn't match its saved pairing."
    static let corruptedKeychainIdentityMessage = "This Mac's pairing key in the Keychain is damaged."
    static let corruptedPairingFileMessage = "The saved pairing file is damaged."

    var errorDescription: String? {
        switch self {
        case .keychainAccessRequired:
            return Self.keychainAccessPromptMessage
        case .keychainSaveFailed:
            return Self.keychainSaveFailedMessage
        case .missingKeychainIdentityForExistingPairing:
            return Self.missingKeychainIdentityMessage
        case .mismatchedKeychainIdentity:
            return Self.mismatchedKeychainIdentityMessage
        case .corruptedKeychainIdentity:
            return Self.corruptedKeychainIdentityMessage
        case .corruptedPairingStateFile:
            return Self.corruptedPairingFileMessage
        }
    }

    var allowsResetPairing: Bool {
        switch self {
        case .missingKeychainIdentityForExistingPairing,
             .mismatchedKeychainIdentity,
             .corruptedKeychainIdentity,
             .corruptedPairingStateFile:
            return true
        case .keychainAccessRequired,
             .keychainSaveFailed:
            return false
        }
    }
}

protocol HostIdentityKeyStore: Sendable {
    func load() -> HostIdentityLoadResult
    @discardableResult func save(_ record: PersistedIdentityRecord) -> Bool
    func wipe()
}

struct KeychainPrimitives: Sendable {
    var copyMatching: @Sendable (CFDictionary, UnsafeMutablePointer<CFTypeRef?>?) -> OSStatus
    var update: @Sendable (CFDictionary, CFDictionary) -> OSStatus
    var add: @Sendable (CFDictionary, UnsafeMutablePointer<CFTypeRef?>?) -> OSStatus
    var delete: @Sendable (CFDictionary) -> OSStatus

    static let system = KeychainPrimitives(
        copyMatching: { query, result in SecItemCopyMatching(query, result) },
        update: { query, attrs in SecItemUpdate(query, attrs) },
        add: { query, result in SecItemAdd(query, result) },
        delete: { query in SecItemDelete(query) }
    )
}

/// Stores the Mac host's pairing private key + self-signed leaf certificate exclusively in the macOS Keychain.
struct KeychainHostIdentityStore: HostIdentityKeyStore {
    static let defaultService = KeychainServicePrefix.current + ".host.pairing"
    static let defaultAccount = "server-identity-v1"

    let service: String
    let account: String
    let primitives: KeychainPrimitives

    init(
        service: String = Self.defaultService,
        account: String = Self.defaultAccount,
        primitives: KeychainPrimitives = .system
    ) {
        self.service = service
        self.account = account
        self.primitives = primitives
    }

    func load() -> HostIdentityLoadResult {
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
              let record = try? JSONDecoder().decode(PersistedIdentityRecord.self, from: data) else {
            return .corruptedData
        }
        return .loaded(record)
    }

    @discardableResult
    func save(_ record: PersistedIdentityRecord) -> Bool {
        guard let data = try? JSONEncoder().encode(record) else { return false }
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
        // Never call SecItemDelete on an update failure other than errSecItemNotFound
        // (e.g. errSecAuthFailed, errSecInteractionNotAllowed, locked Keychain), or an existing pairing key would be destroyed.
        guard updateStatus == errSecItemNotFound else {
            return false
        }
        var addQuery = baseQuery
        addQuery[kSecValueData] = data
        addQuery[kSecAttrAccessible] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        let addStatus = primitives.add(addQuery as CFDictionary, nil)
        return addStatus == errSecSuccess
    }

    func wipe() {
        let query: [CFString: Any] = [
            kSecClass: kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account
        ]
        _ = primitives.delete(query as CFDictionary)
    }
}

/// Throwaway key storage inside an OS temporary folder exclusively for CLI test runs (`kinect-cli`).
/// Refuses any directory outside `FileManager.default.temporaryDirectory` / `/tmp`.
struct TemporaryDirectoryHostIdentityStore: HostIdentityKeyStore {
    let fileURL: URL

    init(directoryURL: URL) throws {
        let stdDir = directoryURL.resolvingSymlinksInPath().standardizedFileURL.path
        let stdTmp = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().standardizedFileURL.path
        guard stdDir.hasPrefix(stdTmp) || stdDir.hasPrefix("/private/tmp/") || stdDir.hasPrefix("/tmp/") else {
            throw NSError(
                domain: "HostStreamServer",
                code: 20,
                userInfo: [NSLocalizedDescriptionKey: "Throwaway CLI key store must reside inside a temporary folder."]
            )
        }
        try FileManager.default.createDirectory(at: directoryURL, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        self.fileURL = directoryURL.appendingPathComponent("throwaway_host_identity.json")
    }

    func load() -> HostIdentityLoadResult {
        guard FileManager.default.fileExists(atPath: fileURL.path) else {
            return .notFound
        }
        guard let data = try? Data(contentsOf: fileURL) else {
            return .accessDeniedOrLocked(errSecIO)
        }
        guard let decoded = try? JSONDecoder().decode(PersistedIdentityRecord.self, from: data) else {
            return .corruptedData
        }
        return .loaded(decoded)
    }

    @discardableResult
    func save(_ record: PersistedIdentityRecord) -> Bool {
        guard let data = try? JSONEncoder().encode(record) else { return false }
        do {
            try data.write(to: fileURL, options: .atomic)
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: fileURL.path)
            return true
        } catch {
            return false
        }
    }

    func wipe() {
        try? FileManager.default.removeItem(at: fileURL)
    }
}

final class InMemoryHostIdentityStore: HostIdentityKeyStore, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: PersistedIdentityRecord?

    init(initial: PersistedIdentityRecord? = nil) {
        self.stored = initial
    }

    func load() -> HostIdentityLoadResult {
        lock.lock()
        defer { lock.unlock() }
        if let stored = stored {
            return .loaded(stored)
        }
        return .notFound
    }

    @discardableResult
    func save(_ record: PersistedIdentityRecord) -> Bool {
        lock.lock()
        stored = record
        lock.unlock()
        return true
    }

    func wipe() {
        lock.lock()
        stored = nil
        lock.unlock()
    }
}

private enum XCTestHostIdentityRegistry {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var storesByPath: [String: InMemoryHostIdentityStore] = [:]

    static func store(forStateDirectory dir: URL) -> InMemoryHostIdentityStore {
        let key = dir.standardizedFileURL.path
        lock.lock()
        defer { lock.unlock() }
        if let existing = storesByPath[key] {
            return existing
        }
        let fresh = InMemoryHostIdentityStore()
        storesByPath[key] = fresh
        return fresh
    }
}

/// On-disk `.host-state/pairing.json` schema.
/// Stores ONLY the public key / certificate SHA-256 fingerprint and pairing metadata — never the private key.
/// Decodes legacy `serverIdentity` once on upgrade so `HostStreamServer` can migrate the key to Keychain and delete it from the file.
struct HostPersistedState: Codable {
    var serverCertSHA256: Data
    var serverPublicKeyDER: Data?
    var serviceId: String
    var pinnedClientCertSHA256: Data?
    var allowPhoneModeSwitch: Bool
    var allowPhoneSensorPowerControl: Bool = true
    var turnOffKinectOnPhoneDisconnect: Bool = false

    /// Present only in memory right after decoding a pre-migration `pairing.json`; never written by `encode(to:)`.
    var legacyServerIdentity: PersistedIdentityRecord?

    enum CodingKeys: String, CodingKey {
        case serverCertSHA256
        case serverPublicKeyDER
        case serviceId
        case pinnedClientCertSHA256
        case allowPhoneModeSwitch
        case allowPhoneSensorPowerControl
        case turnOffKinectOnPhoneDisconnect
        case serverIdentity
    }

    init(
        serverCertSHA256: Data,
        serverPublicKeyDER: Data?,
        serviceId: String,
        pinnedClientCertSHA256: Data?,
        allowPhoneModeSwitch: Bool,
        allowPhoneSensorPowerControl: Bool = true,
        turnOffKinectOnPhoneDisconnect: Bool = false
    ) {
        self.serverCertSHA256 = serverCertSHA256
        self.serverPublicKeyDER = serverPublicKeyDER
        self.serviceId = serviceId
        self.pinnedClientCertSHA256 = pinnedClientCertSHA256
        self.allowPhoneModeSwitch = allowPhoneModeSwitch
        self.allowPhoneSensorPowerControl = allowPhoneSensorPowerControl
        self.turnOffKinectOnPhoneDisconnect = turnOffKinectOnPhoneDisconnect
        self.legacyServerIdentity = nil
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let legacy = try c.decodeIfPresent(PersistedIdentityRecord.self, forKey: .serverIdentity)
        self.legacyServerIdentity = legacy

        if let fp = try c.decodeIfPresent(Data.self, forKey: .serverCertSHA256) {
            self.serverCertSHA256 = fp
        } else if let leg = legacy {
            self.serverCertSHA256 = Data(SHA256.hash(data: leg.certificateDER))
        } else {
            throw DecodingError.keyNotFound(
                CodingKeys.serverCertSHA256,
                DecodingError.Context(codingPath: decoder.codingPath, debugDescription: "Missing serverCertSHA256 and legacy serverIdentity.")
            )
        }

        if let pubDER = try c.decodeIfPresent(Data.self, forKey: .serverPublicKeyDER) {
            self.serverPublicKeyDER = pubDER
        } else if let leg = legacy, let priv = try? P256.Signing.PrivateKey(rawRepresentation: leg.privateKeyRaw) {
            self.serverPublicKeyDER = priv.publicKey.derRepresentation
        } else {
            self.serverPublicKeyDER = nil
        }

        self.serviceId = try c.decode(String.self, forKey: .serviceId)
        self.pinnedClientCertSHA256 = try c.decodeIfPresent(Data.self, forKey: .pinnedClientCertSHA256)
        self.allowPhoneModeSwitch = try c.decode(Bool.self, forKey: .allowPhoneModeSwitch)
        self.allowPhoneSensorPowerControl = try c.decodeIfPresent(Bool.self, forKey: .allowPhoneSensorPowerControl) ?? true
        self.turnOffKinectOnPhoneDisconnect = try c.decodeIfPresent(Bool.self, forKey: .turnOffKinectOnPhoneDisconnect) ?? false
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(serverCertSHA256, forKey: .serverCertSHA256)
        try c.encodeIfPresent(serverPublicKeyDER, forKey: .serverPublicKeyDER)
        try c.encode(serviceId, forKey: .serviceId)
        try c.encodeIfPresent(pinnedClientCertSHA256, forKey: .pinnedClientCertSHA256)
        try c.encode(allowPhoneModeSwitch, forKey: .allowPhoneModeSwitch)
        try c.encode(allowPhoneSensorPowerControl, forKey: .allowPhoneSensorPowerControl)
        try c.encode(turnOffKinectOnPhoneDisconnect, forKey: .turnOffKinectOnPhoneDisconnect)
    }
}

private struct PartialHostPersistedState: Decodable {
    var serverCertSHA256: Data?
    var serverPublicKeyDER: Data?
    var serviceId: String?
    var pinnedClientCertSHA256: Data?
    var allowPhoneModeSwitch: Bool?
    var allowPhoneSensorPowerControl: Bool?
    var turnOffKinectOnPhoneDisconnect: Bool?
    var serverIdentity: PersistedIdentityRecord?
}

struct ActivePairingOffer: Sendable {
    let qrURI: String
    let shortCodeFormatted: String
    let shortCodeNormalized: String
    let qrToken: Data
    let expiresAt: Date
    let port: UInt16
}

final class HostStreamServer: @unchecked Sendable {
    /// `~/kinect-viewer/.host-state`: the host's state folder (pairing.json, presets.json) when none is given.
    static var defaultStateDirectoryURL: URL {
        URL(fileURLWithPath: (NSHomeDirectory() as NSString).appendingPathComponent("kinect-viewer/.host-state"),
            isDirectory: true)
    }

    /// The client certificate a pairing request is checked against: only the one the TLS handshake proved, and only
    /// when the request names that same one. None proved (no TLS metadata, an empty chain) is no pairing: the
    /// certificate a request states is never taken on its word.
    static func pairingClientCertificate(tlsProven: Data?, requestStates: Data) -> Data? {
        guard let proven = tlsProven, !proven.isEmpty, proven == requestStates else { return nil }
        return proven
    }

    private static let netQueueKey = DispatchSpecificKey<UInt8>()
    private let netQueue = DispatchQueue(label: "kinectviewer.host.net", qos: .userInteractive)
    private let stateFileURL: URL
    private let identityStore: HostIdentityKeyStore

    private var state: HostPersistedState
    private var serverIdentity: CryptoIdentity

    static let defaultClientPingTimeoutNs: UInt64 = 5_000_000_000

    private var streamListener: NWListener?
    private var activeClientConn: NWConnection?
    private var activeClientReader = WireFrameReader()
    private var activeClientProtocolVersion: Int = 1
    private var lastClientActivityNs: UInt64 = 0
    private var clientWatchdogTimer: DispatchSourceTimer?
    private var clientPingTimeoutNs: UInt64 = HostStreamServer.defaultClientPingTimeoutNs
    private var simulatedServerProtocolVersion: Int?
    private var isSendingVideo: Bool = false
    private var isShuttingDown: Bool = false

    private var pairingListener: NWListener?
    private var activeOffer: ActivePairingOffer?
    private var pairingExpiryWork: DispatchWorkItem?

    // Diagnostics & Callbacks
    private(set) var streamPort: UInt16 = 0
    private(set) var rejectedHandshakeCount: Int = 0
    private(set) var pairedClientConnected: Bool = false
    private(set) var sentFramesCount: UInt64 = 0
    private(set) var droppedCongestionFrames: UInt64 = 0
    private(set) var lastClientDisplayLatencyMs: Double = 0.0
    private(set) var lastClientBarcodeLatencyMs: Double = 0.0
    private(set) var sentBytesWindow: Int = 0
    private var lastBitrateSampleNs: UInt64 = 0
    private(set) var currentBitrateMbps: Double = 0.0

    var onClientConnectedNeedsKeyframe: (@Sendable () -> HelloServerMessage)?
    var onClientConnectedInitialState: (@Sendable () -> HostStateSyncMessage)?
    var onClientRequestedMode: (@Sendable (Int, Int) -> Void)?
    var onClientRemoteCommand: (@Sendable (RemoteControlCommandMessage) -> Void)?
    var onClientOrbit3DGesture: (@Sendable (Orbit3DGestureMessage) -> Void)?
    var onClientSensorPowerRequest: (@Sendable (SensorPowerControlMessage) -> Void)?
    var onPairedClientDisconnected: (@Sendable () -> Void)?
    var onPairingStateChanged: (@Sendable () -> Void)?

    init(
        stateDirectoryURL: URL? = nil,
        identityStore: HostIdentityKeyStore? = nil,
        fixedStreamPort: UInt16 = 0
    ) throws {
        netQueue.setSpecific(key: Self.netQueueKey, value: 1)
        let dir = stateDirectoryURL ?? Self.defaultStateDirectoryURL
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        self.stateFileURL = dir.appendingPathComponent("pairing.json")

        let isXCTest = ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
        let effectiveStore: HostIdentityKeyStore
        if let explicitStore = identityStore {
            effectiveStore = explicitStore
        } else if isXCTest {
            effectiveStore = XCTestHostIdentityRegistry.store(forStateDirectory: dir)
        } else {
            effectiveStore = KeychainHostIdentityStore()
        }
        self.identityStore = effectiveStore

        let partial: PartialHostPersistedState?
        if FileManager.default.fileExists(atPath: stateFileURL.path) {
            guard let rawFileData = try? Data(contentsOf: stateFileURL),
                  let decoded = try? JSONDecoder().decode(PartialHostPersistedState.self, from: rawFileData) else {
                throw HostPairingKeyError.corruptedPairingStateFile
            }
            partial = decoded
        } else {
            partial = nil
        }

        if let legacyRecord = partial?.serverIdentity {
            // One-time migration: move existing private key into Keychain FIRST, then delete it from pairing.json.
            guard let migratedIdent = try? CryptoIdentity(record: legacyRecord) else {
                throw HostPairingKeyError.corruptedPairingStateFile
            }
            guard effectiveStore.save(migratedIdent.persistedRecord) else {
                throw HostPairingKeyError.keychainSaveFailed
            }
            let svcId = (partial?.serviceId?.isEmpty == false) ? partial!.serviceId! : Self.randomHexId(bytes: 6)
            let migratedState = HostPersistedState(
                serverCertSHA256: migratedIdent.certificateSHA256,
                serverPublicKeyDER: migratedIdent.privateKey.publicKey.derRepresentation,
                serviceId: svcId,
                pinnedClientCertSHA256: partial?.pinnedClientCertSHA256,
                allowPhoneModeSwitch: partial?.allowPhoneModeSwitch ?? true,
                allowPhoneSensorPowerControl: partial?.allowPhoneSensorPowerControl ?? true,
                turnOffKinectOnPhoneDisconnect: partial?.turnOffKinectOnPhoneDisconnect ?? false
            )
            self.state = migratedState
            self.serverIdentity = migratedIdent
            Self.saveStateAtomically(migratedState, to: stateFileURL)
        } else {
            let existingCertHash: Data? = (partial?.serverCertSHA256?.isEmpty == false) ? partial?.serverCertSHA256 : nil
            switch (effectiveStore.load(), existingCertHash) {
            case (.loaded(let storedRecord), .some(let expectedHash)):
                guard let ident = try? CryptoIdentity(record: storedRecord) else {
                    throw HostPairingKeyError.corruptedKeychainIdentity
                }
                guard CryptoIdentity.constantTimeEqual(ident.certificateSHA256, expectedHash) else {
                    throw HostPairingKeyError.mismatchedKeychainIdentity
                }
                guard let svcId = partial?.serviceId, !svcId.isEmpty else {
                    throw HostPairingKeyError.corruptedPairingStateFile
                }
                let loadedState = HostPersistedState(
                    serverCertSHA256: expectedHash,
                    serverPublicKeyDER: partial?.serverPublicKeyDER ?? ident.privateKey.publicKey.derRepresentation,
                    serviceId: svcId,
                    pinnedClientCertSHA256: partial?.pinnedClientCertSHA256,
                    allowPhoneModeSwitch: partial?.allowPhoneModeSwitch ?? true,
                    allowPhoneSensorPowerControl: partial?.allowPhoneSensorPowerControl ?? true,
                    turnOffKinectOnPhoneDisconnect: partial?.turnOffKinectOnPhoneDisconnect ?? false
                )
                if partial?.serverPublicKeyDER == nil {
                    Self.saveStateAtomically(loadedState, to: stateFileURL)
                }
                self.state = loadedState
                self.serverIdentity = ident

            case (.notFound, .none):
                // True first run: Keychain item is not found AND pairing.json has no serverCertSHA256.
                let ident = try CryptoIdentity.generateNew(commonName: "kv2-host")
                guard effectiveStore.save(ident.persistedRecord) else {
                    throw HostPairingKeyError.keychainSaveFailed
                }
                let svcId = (partial?.serviceId?.isEmpty == false) ? partial!.serviceId! : Self.randomHexId(bytes: 6)
                let initial = HostPersistedState(
                    serverCertSHA256: ident.certificateSHA256,
                    serverPublicKeyDER: ident.privateKey.publicKey.derRepresentation,
                    serviceId: svcId,
                    pinnedClientCertSHA256: nil,
                    allowPhoneModeSwitch: partial?.allowPhoneModeSwitch ?? true,
                    allowPhoneSensorPowerControl: partial?.allowPhoneSensorPowerControl ?? true,
                    turnOffKinectOnPhoneDisconnect: partial?.turnOffKinectOnPhoneDisconnect ?? false
                )
                self.state = initial
                self.serverIdentity = ident
                Self.saveStateAtomically(initial, to: stateFileURL)

            case (.notFound, .some):
                // pairing.json has a server cert, so missing Keychain item is an error, never a first run.
                throw HostPairingKeyError.missingKeychainIdentityForExistingPairing

            case (.accessDeniedOrLocked(let status), _):
                throw HostPairingKeyError.keychainAccessRequired(status)

            case (.corruptedData, _):
                throw HostPairingKeyError.corruptedKeychainIdentity

            case (.loaded(let storedRecord), .none):
                // Keychain item survived a deleted pairing.json / state folder or reinstall, and no iPhone is pinned:
                // adopt the existing Keychain identity and write a fresh pairing.json keeping any partial settings.
                guard let ident = try? CryptoIdentity(record: storedRecord) else {
                    throw HostPairingKeyError.corruptedKeychainIdentity
                }
                let svcId = (partial?.serviceId?.isEmpty == false) ? partial!.serviceId! : Self.randomHexId(bytes: 6)
                let adopted = HostPersistedState(
                    serverCertSHA256: ident.certificateSHA256,
                    serverPublicKeyDER: ident.privateKey.publicKey.derRepresentation,
                    serviceId: svcId,
                    pinnedClientCertSHA256: nil,
                    allowPhoneModeSwitch: partial?.allowPhoneModeSwitch ?? true,
                    allowPhoneSensorPowerControl: partial?.allowPhoneSensorPowerControl ?? true,
                    turnOffKinectOnPhoneDisconnect: partial?.turnOffKinectOnPhoneDisconnect ?? false
                )
                self.state = adopted
                self.serverIdentity = ident
                Self.saveStateAtomically(adopted, to: stateFileURL)
            }
        }

        try startStreamListener(preferredPort: fixedStreamPort)
    }

    /// Explicitly invoked ONLY after user confirmation on an unrecoverable pairing key/file mismatch or corruption.
    /// Preserves a `.bad` copy of any existing `pairing.json` first, creates and saves a fresh Keychain identity,
    /// and writes a clean unpaired `pairing.json`.
    static func resetPairingAfterBackup(
        stateDirectoryURL: URL? = nil,
        identityStore: HostIdentityKeyStore? = nil
    ) throws {
        let dir = stateDirectoryURL ?? Self.defaultStateDirectoryURL
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let stateFileURL = dir.appendingPathComponent("pairing.json")
        let badBackupURL = stateFileURL.appendingPathExtension("bad")

        let isXCTest = ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
        let effectiveStore: HostIdentityKeyStore
        if let explicitStore = identityStore {
            effectiveStore = explicitStore
        } else if isXCTest {
            effectiveStore = XCTestHostIdentityRegistry.store(forStateDirectory: dir)
        } else {
            effectiveStore = KeychainHostIdentityStore()
        }

        var preservedAllowSwitch = true
        var preservedAllowPower = true
        var preservedTurnOffOnDisconnect = false

        if FileManager.default.fileExists(atPath: stateFileURL.path) {
            let existingBytes = try Data(contentsOf: stateFileURL)
            if let readablePartial = try? JSONDecoder().decode(PartialHostPersistedState.self, from: existingBytes) {
                preservedAllowSwitch = readablePartial.allowPhoneModeSwitch ?? true
                preservedAllowPower = readablePartial.allowPhoneSensorPowerControl ?? true
                preservedTurnOffOnDisconnect = readablePartial.turnOffKinectOnPhoneDisconnect ?? false
            }
            try existingBytes.write(to: badBackupURL, options: .atomic)
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: badBackupURL.path)
        }

        let freshIdent = try CryptoIdentity.generateNew(commonName: "kv2-host")
        guard effectiveStore.save(freshIdent.persistedRecord) else {
            throw HostPairingKeyError.keychainSaveFailed
        }

        let freshState = HostPersistedState(
            serverCertSHA256: freshIdent.certificateSHA256,
            serverPublicKeyDER: freshIdent.privateKey.publicKey.derRepresentation,
            serviceId: Self.randomHexId(bytes: 6),
            pinnedClientCertSHA256: nil,
            allowPhoneModeSwitch: preservedAllowSwitch,
            allowPhoneSensorPowerControl: preservedAllowPower,
            turnOffKinectOnPhoneDisconnect: preservedTurnOffOnDisconnect
        )
        Self.saveStateAtomically(freshState, to: stateFileURL)
    }

    private func syncOnNetQueue<T>(_ body: () -> T) -> T {
        if DispatchQueue.getSpecific(key: Self.netQueueKey) != nil {
            return body()
        }
        return netQueue.sync(execute: body)
    }

    var isPhonePaired: Bool {
        syncOnNetQueue { state.pinnedClientCertSHA256 != nil }
    }

    var pinnedClientFingerprintShort: String {
        syncOnNetQueue {
            guard let fp = state.pinnedClientCertSHA256 else { return "None" }
            return fp.prefix(6).map { String(format: "%02x", $0) }.joined()
        }
    }

    var serviceId: String {
        syncOnNetQueue { state.serviceId }
    }

    var serverCertSHA256: Data {
        serverIdentity.certificateSHA256
    }

    var allowPhoneModeSwitch: Bool {
        get { syncOnNetQueue { state.allowPhoneModeSwitch } }
        set {
            netQueue.async {
                guard self.state.allowPhoneModeSwitch != newValue else { return }
                self.state.allowPhoneModeSwitch = newValue
                Self.saveStateAtomically(self.state, to: self.stateFileURL)
                if let hello = self.onClientConnectedNeedsKeyframe?() {
                    self.sendHelloToActiveClientOnQueue(hello)
                }
                if let stateSync = self.onClientConnectedInitialState?() {
                    self.sendHostStateToActiveClientOnQueue(stateSync)
                }
            }
        }
    }

    var allowPhoneSensorPowerControl: Bool {
        get { syncOnNetQueue { state.allowPhoneSensorPowerControl } }
        set {
            netQueue.async {
                guard self.state.allowPhoneSensorPowerControl != newValue else { return }
                self.state.allowPhoneSensorPowerControl = newValue
                Self.saveStateAtomically(self.state, to: self.stateFileURL)
                if let stateSync = self.onClientConnectedInitialState?() {
                    self.sendHostStateToActiveClientOnQueue(stateSync)
                }
            }
        }
    }

    var turnOffKinectOnPhoneDisconnect: Bool {
        get { syncOnNetQueue { state.turnOffKinectOnPhoneDisconnect } }
        set {
            netQueue.async {
                guard self.state.turnOffKinectOnPhoneDisconnect != newValue else { return }
                self.state.turnOffKinectOnPhoneDisconnect = newValue
                Self.saveStateAtomically(self.state, to: self.stateFileURL)
            }
        }
    }

    var configuredClientPingTimeoutSeconds: Double {
        syncOnNetQueue { Double(clientPingTimeoutNs) / 1e9 }
    }

    func setClientPingTimeoutForTesting(seconds: Double) {
        let clampedNs = UInt64(max(0.1, seconds) * 1_000_000_000.0)
        syncOnNetQueue {
            clientPingTimeoutNs = clampedNs
        }
    }

    func setSimulatedServerProtocolVersionForTesting(_ version: Int?) {
        syncOnNetQueue {
            simulatedServerProtocolVersion = version
            if let v = version, v < 2 {
                activeClientProtocolVersion = 1
            }
        }
    }

    func shutdown() {
        syncOnNetQueue {
            isShuttingDown = true
            closePairingWindowOnQueue()
            stopClientWatchdogOnQueue()
            activeClientConn?.cancel()
            activeClientConn = nil
            activeClientProtocolVersion = 1
            pairedClientConnected = false
            streamListener?.cancel()
            streamListener = nil
        }
    }

    func disconnectActiveClient() {
        var didDisconnect = false
        syncOnNetQueue {
            didDisconnect = pairedClientConnected
            stopClientWatchdogOnQueue()
            activeClientConn?.cancel()
            activeClientConn = nil
            activeClientProtocolVersion = 1
            pairedClientConnected = false
            isSendingVideo = false
        }
        if didDisconnect && !isShuttingDown {
            onPairedClientDisconnected?()
        }
        onPairingStateChanged?()
    }

    func unpairPhone() {
        disconnectActiveClient()
        syncOnNetQueue {
            state.pinnedClientCertSHA256 = nil
            state.serviceId = Self.randomHexId(bytes: 6)
            Self.saveStateAtomically(state, to: stateFileURL)
            updateBonjourAdvertisementOnQueue()
        }
        onPairingStateChanged?()
    }

    // MARK: - Main Mutually-Authenticated TLS 1.3 Stream Listener

    private func startStreamListener(preferredPort: UInt16) throws {
        let params = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { [weak self] peerHash, _ in
            guard let self = self else { return false }
            // Verify runs on netQueue: reject immediately inside TLS 1.3 handshake unless exact pinned phone matches!
            guard let pinned = self.state.pinnedClientCertSHA256 else {
                self.rejectedHandshakeCount += 1
                return false
            }
            let matched = CryptoIdentity.constantTimeEqual(peerHash, pinned)
            if !matched {
                self.rejectedHandshakeCount += 1
            }
            return matched
        }

        let listener: NWListener
        if preferredPort > 0, let nwPort = NWEndpoint.Port(rawValue: preferredPort) {
            listener = try NWListener(using: params, on: nwPort)
        } else {
            listener = try NWListener(using: params)
        }

        // Only advertise opaque Bonjour ID when a phone is actually paired!
        if state.pinnedClientCertSHA256 != nil {
            listener.service = NWListener.Service(name: state.serviceId, type: BonjourConstants.streamServiceType)
        }

        let readySem = DispatchSemaphore(value: 0)
        listener.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            switch st {
            case .ready:
                if let p = listener.port?.rawValue {
                    self.streamPort = p
                }
                readySem.signal()
            default:
                break
            }
        }

        listener.newConnectionHandler = { [weak self] conn in
            self?.handleIncomingStreamConnectionOnQueue(conn)
        }

        self.streamListener = listener
        listener.start(queue: netQueue)
        _ = readySem.wait(timeout: .now() + 2.0)
    }

    private func updateBonjourAdvertisementOnQueue() {
        guard let listener = streamListener else { return }
        if state.pinnedClientCertSHA256 != nil {
            listener.service = NWListener.Service(name: state.serviceId, type: BonjourConstants.streamServiceType)
        } else {
            listener.service = nil
        }
    }

    private func startClientWatchdogOnQueue(for conn: NWConnection) {
        stopClientWatchdogOnQueue()
        let timer = DispatchSource.makeTimerSource(queue: netQueue)
        timer.schedule(deadline: .now() + .milliseconds(200), repeating: .milliseconds(200))
        timer.setEventHandler { [weak self] in
            guard let self = self,
                  self.activeClientConn === conn,
                  self.pairedClientConnected else { return }
            let nowNs = kv_monotonic_ns()
            if nowNs > self.lastClientActivityNs,
               (nowNs - self.lastClientActivityNs) > self.clientPingTimeoutNs {
                // Dead peer detected (no ping/frame within timeout ~5s): cancel so disconnect hooks trigger
                self.stopClientWatchdogOnQueue()
                conn.cancel()
            }
        }
        self.clientWatchdogTimer = timer
        timer.resume()
    }

    private func stopClientWatchdogOnQueue() {
        clientWatchdogTimer?.cancel()
        clientWatchdogTimer = nil
    }

    private func handleIncomingStreamConnectionOnQueue(_ conn: NWConnection) {
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            switch st {
            case .ready:
                // Only reached AFTER mutual TLS 1.3 pinned client-certificate verification succeeded!
                if let existing = self.activeClientConn, existing !== conn {
                    existing.cancel()
                }
                self.activeClientConn = conn
                self.activeClientReader.reset()
                // Start strictly in protocol v1 compatibility mode; never send 0x07 until client announces v2!
                self.activeClientProtocolVersion = 1
                self.lastClientActivityNs = kv_monotonic_ns()
                self.isSendingVideo = false
                self.pairedClientConnected = true
                self.startClientWatchdogOnQueue(for: conn)
                if let hello = self.onClientConnectedNeedsKeyframe?() {
                    self.sendHelloToActiveClientOnQueue(hello)
                }
                self.receiveStreamLoopOnQueue(conn)
                self.onPairingStateChanged?()

            case .failed, .cancelled:
                if self.activeClientConn === conn {
                    let wasConnected = self.pairedClientConnected
                    self.stopClientWatchdogOnQueue()
                    self.activeClientConn = nil
                    self.activeClientProtocolVersion = 1
                    self.pairedClientConnected = false
                    self.isSendingVideo = false
                    if wasConnected && !self.isShuttingDown {
                        self.onPairedClientDisconnected?()
                    }
                    self.onPairingStateChanged?()
                }
            default:
                break
            }
        }
        conn.start(queue: netQueue)
    }

    private func sendHelloToActiveClientOnQueue(_ hello: HelloServerMessage) {
        guard let conn = activeClientConn else { return }
        let effectiveHello: HelloServerMessage
        if let forcedVer = simulatedServerProtocolVersion {
            effectiveHello = HelloServerMessage(
                protocolVersion: forcedVer,
                modeRaw: hello.modeRaw,
                sbsLayoutRaw: hello.sbsLayoutRaw,
                allowPhoneModeSwitch: hello.allowPhoneModeSwitch,
                width: hello.width,
                height: hello.height,
                codecRaw: hello.codecRaw
            )
        } else {
            effectiveHello = hello
        }
        guard let packet = try? WirePacket.packJSON(type: .helloServer, effectiveHello) else { return }
        conn.send(content: packet, completion: .contentProcessed { _ in })
    }

    private func sendHostStateToActiveClientOnQueue(_ stateSync: HostStateSyncMessage) {
        guard activeClientProtocolVersion >= 2,
              (simulatedServerProtocolVersion ?? 2) >= 2,
              let conn = activeClientConn,
              let packet = try? WirePacket.packJSON(type: .hostStateSync, stateSync) else { return }
        conn.send(content: packet, completion: .contentProcessed { _ in })
    }

    func notifyModeChanged(_ hello: HelloServerMessage) {
        netQueue.async {
            self.sendHelloToActiveClientOnQueue(hello)
        }
    }

    func broadcastHostState(_ stateSync: HostStateSyncMessage) {
        netQueue.async {
            self.sendHostStateToActiveClientOnQueue(stateSync)
        }
    }

    func broadcastEncodedFrame(_ frame: EncodedVideoFrame, onDroppedForBackpressure: (@Sendable () -> Void)?) {
        netQueue.async {
            guard let conn = self.activeClientConn, self.pairedClientConnected else { return }

            // Always deliver StreamConfig immediately so parameter sets stay synchronized
            if let cfg = frame.configIfChanged,
               let cfgPkt = try? WirePacket.packJSON(type: .streamConfig, cfg) {
                conn.send(content: cfgPkt, completion: .contentProcessed { _ in })
            }

            if self.isSendingVideo && !frame.header.isKeyframe {
                self.droppedCongestionFrames += 1
                onDroppedForBackpressure?()
                return
            }

            let pkt = WirePacket.pack(type: .videoAccessUnit, flags: frame.header.flags, payload: frame.wirePayload)
            self.isSendingVideo = true
            self.sentBytesWindow += pkt.count
            let nowNs = kv_monotonic_ns()
            if self.lastBitrateSampleNs == 0 {
                self.lastBitrateSampleNs = nowNs
            } else if nowNs - self.lastBitrateSampleNs >= 1_000_000_000 {
                let dtSec = Double(nowNs - self.lastBitrateSampleNs) / 1e9
                self.currentBitrateMbps = (Double(self.sentBytesWindow) * 8.0) / (dtSec * 1_000_000.0)
                self.sentBytesWindow = 0
                self.lastBitrateSampleNs = nowNs
            }

            conn.send(content: pkt, completion: .contentProcessed { [weak self] _ in
                guard let self = self else { return }
                self.isSendingVideo = false
                self.sentFramesCount += 1
            })
        }
    }

    private func receiveStreamLoopOnQueue(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, err in
            guard let self = self, self.activeClientConn === conn else { return }
            if let data = data, !data.isEmpty {
                self.lastClientActivityNs = kv_monotonic_ns()
                self.activeClientReader.append(data)
                let strictV1Mode = (self.simulatedServerProtocolVersion ?? 2) < 2
                while true {
                    switch self.activeClientReader.nextFrame() {
                    case .needMoreData:
                        break
                    case .protocolViolation:
                        conn.cancel()
                        return
                    case .unknownFrame:
                        if strictV1Mode {
                            conn.cancel()
                            return
                        }
                        // Additive forward compatibility: ignore unrecognized message types cleanly
                        continue
                    case .frame(let msgType, _, let payload):
                        if strictV1Mode && msgType.rawValue >= WireMessageType.hostStateSync.rawValue {
                            conn.cancel()
                            return
                        }
                        self.handleClientFrameOnQueue(conn: conn, type: msgType, payload: payload)
                        continue
                    }
                    break
                }
            }
            if isComplete || err != nil {
                conn.cancel()
                return
            }
            self.receiveStreamLoopOnQueue(conn)
        }
    }

    private func handleClientFrameOnQueue(conn: NWConnection, type: WireMessageType, payload: Data) {
        switch type {
        case .ping:
            let recvNs = kv_monotonic_ns()
            guard let ping = try? JSONDecoder().decode(PingMessage.self, from: payload) else { return }
            if let clientVer = ping.protocolVersion,
               clientVer >= 2,
               activeClientProtocolVersion < 2,
               (simulatedServerProtocolVersion ?? 2) >= 2 {
                activeClientProtocolVersion = clientVer
                if let stateSync = self.onClientConnectedInitialState?() {
                    self.sendHostStateToActiveClientOnQueue(stateSync)
                }
            }
            if ping.lastHostToDisplayLatencyUs > 0 {
                self.lastClientDisplayLatencyMs = Double(ping.lastHostToDisplayLatencyUs) / 1000.0
            }
            if ping.lastBarcodeLatencyUs > 0 {
                self.lastClientBarcodeLatencyMs = Double(ping.lastBarcodeLatencyUs) / 1000.0
            }
            let pong = PongMessage(
                clientSendNs: ping.clientSendNs,
                serverRecvNs: recvNs,
                serverSendNs: kv_monotonic_ns()
            )
            if let pkt = try? WirePacket.packJSON(type: .pong, pong) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }

        case .selectModeReq:
            guard state.allowPhoneModeSwitch,
                  let req = try? JSONDecoder().decode(SelectModeMessage.self, from: payload) else { return }
            onClientRequestedMode?(req.modeRaw, req.sbsLayoutRaw)

        case .remoteControlCmd:
            guard let cmd = try? JSONDecoder().decode(RemoteControlCommandMessage.self, from: payload) else { return }
            if cmd.action != .requestKeyframe && !state.allowPhoneModeSwitch {
                return
            }
            onClientRemoteCommand?(cmd)

        case .orbit3DGesture:
            guard state.allowPhoneModeSwitch,
                  let orbit = try? JSONDecoder().decode(Orbit3DGestureMessage.self, from: payload) else { return }
            onClientOrbit3DGesture?(orbit)

        case .sensorPowerReq:
            guard let powerReq = try? JSONDecoder().decode(SensorPowerControlMessage.self, from: payload) else { return }
            guard state.allowPhoneSensorPowerControl else {
                if let stateSync = self.onClientConnectedInitialState?() {
                    self.sendHostStateToActiveClientOnQueue(stateSync)
                }
                return
            }
            onClientSensorPowerRequest?(powerReq)

        default:
            // Any unexpected message type is ignored; recording download commands do not exist in protocol.
            break
        }
    }

    // MARK: - Time-Boxed One-Time Pairing Listener (QR & Short Code)

    func beginPairingWindow(durationSec: TimeInterval = 120.0) throws -> ActivePairingOffer {
        var createdOffer: ActivePairingOffer?
        var thrownErr: Error?

        syncOnNetQueue {
            closePairingWindowOnQueue()

            var qrTokBytes = [UInt8](repeating: 0, count: 32)
            _ = SecRandomCopyBytes(kSecRandomDefault, qrTokBytes.count, &qrTokBytes)
            let qrToken = Data(qrTokBytes)
            let codePair = QRPairingPayload.generateShortCode()

            let tlsParams = CryptoIdentity.makeTLSParameters(
                localIdentity: serverIdentity,
                requireClientCert: true,
                verifyQueue: netQueue
            ) { _, _ in
                // Accept self-signed client leaf for the pairing exchange ONLY; proof is verified via transcript-bound HMAC
                return true
            }

            do {
                let pListener = try NWListener(using: tlsParams)
                pListener.service = NWListener.Service(name: state.serviceId, type: BonjourConstants.pairingServiceType)

                let readySem = DispatchSemaphore(value: 0)
                pListener.stateUpdateHandler = { st in
                    if case .ready = st {
                        readySem.signal()
                    }
                }
                pListener.newConnectionHandler = { [weak self] conn in
                    self?.handleIncomingPairingConnectionOnQueue(conn)
                }

                self.pairingListener = pListener
                pListener.start(queue: DispatchQueue.global(qos: .userInitiated))
                _ = readySem.wait(timeout: .now() + 2.0)
                let boundPort = pListener.port?.rawValue ?? 0

                let hostIP = Self.localWiFiIPv4Address()
                let qr = QRPairingPayload(
                    serviceId: state.serviceId,
                    serverCertSHA256: serverIdentity.certificateSHA256,
                    oneTimeToken: qrToken,
                    hostIP: hostIP,
                    pairPort: boundPort
                )
                let offer = ActivePairingOffer(
                    qrURI: qr.toURIString(),
                    shortCodeFormatted: codePair.formatted,
                    shortCodeNormalized: codePair.normalized,
                    qrToken: qrToken,
                    expiresAt: Date().addingTimeInterval(durationSec),
                    port: boundPort
                )
                self.activeOffer = offer
                createdOffer = offer

                let work = DispatchWorkItem { [weak self] in
                    self?.closePairingWindowOnQueue()
                    self?.onPairingStateChanged?()
                }
                self.pairingExpiryWork = work
                netQueue.asyncAfter(deadline: .now() + durationSec, execute: work)
            } catch {
                thrownErr = error
            }
        }

        if let err = thrownErr { throw err }
        return createdOffer!
    }

    func cancelPairingWindow() {
        syncOnNetQueue {
            closePairingWindowOnQueue()
        }
        onPairingStateChanged?()
    }

    var currentPairingOffer: ActivePairingOffer? {
        syncOnNetQueue { activeOffer }
    }

    private func closePairingWindowOnQueue() {
        pairingExpiryWork?.cancel()
        pairingExpiryWork = nil
        activeOffer = nil
        pairingListener?.cancel()
        pairingListener = nil
    }

    private func handleIncomingPairingConnectionOnQueue(_ conn: NWConnection) {
        let reader = WireFrameReader()
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                self.receivePairingFrameOnQueue(conn: conn, reader: reader)
            }
        }
        conn.start(queue: netQueue)
    }

    private func receivePairingFrameOnQueue(conn: NWConnection, reader: WireFrameReader) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, isComplete, err in
            guard let self = self else { return }
            if let data = data, !data.isEmpty {
                reader.append(data)
                switch reader.nextFrame() {
                case .needMoreData:
                    if !isComplete && err == nil {
                        self.receivePairingFrameOnQueue(conn: conn, reader: reader)
                    } else {
                        conn.cancel()
                    }
                case .protocolViolation, .unknownFrame:
                    conn.cancel()
                case .frame(let type, _, let payload):
                    guard type == .pairRequest,
                          let req = try? JSONDecoder().decode(PairRequestMessage.self, from: payload),
                          let offer = self.activeOffer,
                          Date() < offer.expiresAt else {
                        conn.cancel()
                        return
                    }
                    self.processPairRequestOnQueue(conn: conn, req: req, offer: offer)
                }
            } else if isComplete || err != nil {
                conn.cancel()
            }
        }
    }

    private func processPairRequestOnQueue(conn: NWConnection, req: PairRequestMessage, offer: ActivePairingOffer) {
        // Extract actual TLS-authenticated peer leaf cert from NWConnection metadata to prevent cert spoofing
        var tlsPeerCertDER: Data?
        if let tlsMeta = conn.metadata(definition: NWProtocolTLS.definition) as? NWProtocolTLS.Metadata {
            let secMeta = tlsMeta.securityProtocolMetadata
            sec_protocol_metadata_access_peer_certificate_chain(secMeta) { secCert in
                if tlsPeerCertDER == nil {
                    let cfCert = sec_certificate_copy_ref(secCert).takeRetainedValue()
                    tlsPeerCertDER = SecCertificateCopyData(cfCert) as Data
                }
            }
        }

        // Fails closed, as the Windows host does: no certificate the handshake proved, or one other than the request
        // names, closes the pairing window.
        guard let verifiedClientDER = Self.pairingClientCertificate(
            tlsProven: tlsPeerCertDER,
            requestStates: req.clientCertDER
        ) else {
            closePairingWindowOnQueue()
            conn.cancel()
            onPairingStateChanged?()
            return
        }

        let clientCertHash = Data(SHA256.hash(data: verifiedClientDER))
        let secret: Data = (req.methodRaw == WirePairingMethod.qrToken.rawValue)
            ? offer.qrToken
            : Data(offer.shortCodeNormalized.utf8)

        let expectedClientProof = CryptoIdentity.computePairingProof(
            secret: secret,
            serverCertSHA256: serverIdentity.certificateSHA256,
            clientCertSHA256: clientCertHash,
            role: "kv2-pair-client"
        )

        guard CryptoIdentity.constantTimeEqual(req.clientProofHMAC, expectedClientProof) else {
            // Single-attempt enforcement: immediately burn the short code / pairing offer on wrong guess!
            let reject = PairResponseMessage(
                accepted: false,
                serviceId: "",
                streamPort: 0,
                serverProofHMAC: Data(),
                message: "Invalid pairing code or token; pairing window closed."
            )
            if let pkt = try? WirePacket.packJSON(type: .pairResponse, reject) {
                conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
            } else {
                conn.cancel()
            }
            closePairingWindowOnQueue()
            onPairingStateChanged?()
            return
        }

        // Valid pairing! Pin this single phone's certificate and immediately shut down pairing window.
        state.pinnedClientCertSHA256 = clientCertHash
        Self.saveStateAtomically(state, to: stateFileURL)
        updateBonjourAdvertisementOnQueue()

        let serverProof = CryptoIdentity.computePairingProof(
            secret: secret,
            serverCertSHA256: serverIdentity.certificateSHA256,
            clientCertSHA256: clientCertHash,
            role: "kv2-pair-server"
        )
        let resp = PairResponseMessage(
            accepted: true,
            serviceId: state.serviceId,
            streamPort: streamPort,
            serverProofHMAC: serverProof,
            message: "Paired"
        )
        closePairingWindowOnQueue()

        if let pkt = try? WirePacket.packJSON(type: .pairResponse, resp) {
            conn.send(content: pkt, completion: .contentProcessed { _ in
                conn.cancel()
            })
        } else {
            conn.cancel()
        }
        onPairingStateChanged?()
    }

    // MARK: - Helpers

    private static func saveStateAtomically(_ state: HostPersistedState, to url: URL) {
        guard let data = try? JSONEncoder().encode(state) else { return }
        try? data.write(to: url, options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }

    private static func randomHexId(bytes count: Int) -> String {
        var b = [UInt8](repeating: 0, count: count)
        _ = SecRandomCopyBytes(kSecRandomDefault, count, &b)
        return b.map { String(format: "%02x", $0) }.joined()
    }

    static func localWiFiIPv4Address() -> String {
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else {
            return "127.0.0.1"
        }
        defer { freeifaddrs(ifaddr) }

        var fallback: String?
        var ptr: UnsafeMutablePointer<ifaddrs>? = first
        while let cur = ptr {
            let flags = Int32(cur.pointee.ifa_flags)
            let isUp = (flags & IFF_UP) != 0 && (flags & IFF_LOOPBACK) == 0
            if isUp, let addr = cur.pointee.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) {
                let name = String(cString: cur.pointee.ifa_name)
                var hostBuf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &hostBuf, socklen_t(hostBuf.count), nil, 0, NI_NUMERICHOST) == 0 {
                    let ip = hostBuf.withUnsafeBufferPointer { buf in
                        String(cString: buf.baseAddress!)
                    }
                    if !ip.hasPrefix("169.254.") {
                        if name == "en0" || name == "en1" {
                            return ip
                        }
                        if fallback == nil {
                            fallback = ip
                        }
                    }
                }
            }
            ptr = cur.pointee.ifa_next
        }
        return fallback ?? "127.0.0.1"
    }

    static func makeQRCodeCGImage(from text: String, scale: CGFloat = 8.0) -> CGImage? {
        guard let filter = CIFilter(name: "CIQRCodeGenerator") else { return nil }
        filter.setValue(Data(text.utf8), forKey: "inputMessage")
        filter.setValue("M", forKey: "inputCorrectionLevel")
        guard let ciImg = filter.outputImage else { return nil }
        let transformed = ciImg.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let ctx = CIContext(options: [.useSoftwareRenderer: false])
        return ctx.createCGImage(transformed, from: transformed.extent)
    }
}
