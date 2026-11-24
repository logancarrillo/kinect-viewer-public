import Foundation

/// The prefix of the Keychain service names the apps keep their pairing keys under: the build's
/// `KV_BUNDLE_ID_PREFIX`, which each app's Info.plist carries as `KVBundleIDPrefix` (project.yml).
///
/// A fresh clone builds as `com.example.kinectviewer` (Config/Base.xcconfig). A Mac or iPhone built with its own
/// prefix in Config/Local.xcconfig gets service names under that prefix, so a build with the prefix an install
/// already used keeps the pairing keys it has, and nothing has to pair again.
enum KeychainServicePrefix {
    /// Config/Base.xcconfig's own default, for a bundle without the key (a command-line tool, say).
    static let fallback = "com.example.kinectviewer"

    static var current: String {
        resolve(infoValue: Bundle.main.object(forInfoDictionaryKey: "KVBundleIDPrefix") as? String)
    }

    /// The Info.plist value when it is a usable prefix; nothing, an empty value, or a build setting left
    /// unexpanded (`$(…)`) gives the fallback.
    static func resolve(infoValue: String?) -> String {
        guard let value = infoValue?.trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty,
              !value.contains("$("),
              !value.hasPrefix("."),
              !value.hasSuffix(".") else {
            return fallback
        }
        return value
    }
}
