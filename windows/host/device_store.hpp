// The paired-device list (protocol v3 §2.1, pet-camera.md §5): devices.json next to pairing.json,
// the start-up reconciliation with pairing.json, and the downgrade mirror.
//
// pairing.json keeps its keys. Its pinnedClientCertSHA256 becomes a write-only "downgrade mirror":
// the hash of the most recently paired (or re-paired) owner, or no key when there is no owner. Only
// loadAndReconcile reads it. Admission, advertising and the "paired" state use the device list.
//
// Thread safety: none. DeviceStore is a plain value; the caller holds its server lock around every
// call (revoke and on_ready must run under the same lock, v3 §3.2).
#pragma once

#include "host_state.hpp"
#include "pet_role.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvhost {

constexpr size_t kMaxPairedDevices = 16;   // v3 §2.1: a 17th pairing is refused
constexpr size_t kMaxRevokedHashes = 256;  // pet-camera.md §5: oldest dropped first
constexpr size_t kMaxDeviceNameBytes = 64; // v3 §4.4

enum class DeviceKind { IPhone, IPad, Mac, Other };
const char* deviceKindString(DeviceKind k);       // "iphone", "ipad", "mac", "other"
DeviceKind deviceKindFromString(const std::string& s); // v3 §4.4: any other value becomes Other

struct DeviceRecord {
    std::vector<uint8_t> certSHA256; // the full 32-byte leaf certificate hash
    std::string deviceId;            // first 8 bytes of certSHA256, 16 lowercase hex characters
    std::string name;                // sanitised, 1-64 bytes of UTF-8
    PetRole role = PetRole::Viewer;  // Owner or Viewer, never Unknown
    DeviceKind kind = DeviceKind::Other;
    uint64_t pairedAtUnixMs = 0; // also set by a re-pair
    std::optional<uint64_t> lastSeenUnixMs;
    std::optional<int> lastProtocolVersion;
};

// The two pairing.json values devices.json tracks: "written" and "before" (pet-camera.md §5).
struct PairingSnapshot {
    std::string serviceId;
    std::optional<std::vector<uint8_t>> mirror;
    bool operator==(const PairingSnapshot& o) const { return serviceId == o.serviceId && mirror == o.mirror; }
    bool operator!=(const PairingSnapshot& o) const { return !(*this == o); }
};

// "<16 hex>" from the first 8 bytes of the hash (fewer if the hash is shorter).
std::string deviceIdFromHash(const std::vector<uint8_t>& certSHA256);

// v3 §4.4: invalid UTF-8 becomes U+FFFD, control characters (C0, DEL, C1, U+2028/2029) become spaces, the
// bidirectional controls (U+061C, U+200E/200F, U+202A-202E, U+2066-2069) and the characters that show nothing
// (U+00AD, U+180E, U+200B, U+2060-2064, U+FEFF) are dropped, leading and trailing whitespace is trimmed, and more than
// 64 bytes is cut at a character boundary.
// nullopt when nothing is left, so the caller uses a default name.
std::optional<std::string> sanitizeDeviceName(const std::string& raw);

// `name` if it isn't in `taken` (exact match), else "<name> 2", "<name> 3", ...: the first free one, the name cut
// at a character boundary so that it and the number fit in 64 bytes. A newly paired device's name goes through it.
std::string uniqueDeviceName(const std::string& name, const std::vector<std::string>& taken);
// uniqueDeviceName("iPhone", taken).
std::string nextDefaultDeviceName(const std::vector<std::string>& taken);

class DeviceStore {
public:
    explicit DeviceStore(std::wstring stateDir);

    // ---- start-up ------------------------------------------------------------------------------

    enum class ReconcileCase {
        Migrated,        // row 1: no devices.json (or one that didn't parse, once kept as devices.json.bad)
        Consistent,      // row 2: (P, S) == written
        RepairedPairing, // row 3: (P, S) == before, pairing.json rewritten from written
        DowngradeUnpair, // row 4a: serviceId changed
        DowngradePair,   // row 4b: same serviceId, a different pin
        DowngradeClear,  // row 4c: same serviceId, pin cleared
        // devices.json exists but couldn't be read (another program holds it), or didn't parse and couldn't
        // be kept as devices.json.bad: the list is empty, nothing was written, and pairing is untouched.
        Unread,
    };

    struct ReconcileResult {
        ReconcileCase which = ReconcileCase::Consistent;
        std::vector<std::string> log; // human-readable, one line per action
        bool pairingRewritten = false;
        bool devicesRewritten = false;
        std::string saveError; // non-empty when a repair write failed; the list in memory is still valid
        // Unread: nothing may be admitted or saved until a later loadAndReconcile reads the file.
        bool readFailed = false;
        std::string readError; // a sentence for the PC when readFailed
    };

    // Reads devices.json (leniently) and reconciles it with `pairing`, the state just loaded (or just
    // regenerated) from pairing.json, applying pet-camera.md §5 rows 1-4c in order. Repairs both files
    // when needed and sets pairing.serviceId / pairing.pinnedClientCertSHA256 to what is on disk.
    ReconcileResult loadAndReconcile(HostPersistedState& pairing, uint64_t nowUnixMs);

    // ---- queries -------------------------------------------------------------------------------

    const std::vector<DeviceRecord>& devices() const { return devices_; }
    const std::vector<std::vector<uint8_t>>& revoked() const { return revoked_; } // oldest first
    size_t size() const { return devices_.size(); }
    bool empty() const { return devices_.empty(); }
    bool full() const { return devices_.size() >= kMaxPairedDevices; }

    // Compares against every listed hash in constant time (for admission in the TLS handshake).
    const DeviceRecord* findByHash(const std::vector<uint8_t>& certSHA256) const;
    const DeviceRecord* findByDeviceId(const std::string& deviceId) const;
    bool isRevoked(const std::vector<uint8_t>& certSHA256) const;

    // The downgrade mirror: the hash of the owner with the latest pairedAtUnixMs (on a tie, the one
    // later in the list), or nullopt when there is no owner.
    std::optional<std::vector<uint8_t>> mirror() const;

    std::string nextDefaultName() const;

    // ---- changes (in memory; call save() afterwards) -----------------------------------------

    struct NewDevice {
        std::vector<uint8_t> certSHA256;
        PetRole role = PetRole::Viewer;          // chosen on the PC (D1); Unknown is refused
        std::optional<std::string> deviceName;   // pairRequest.deviceName, raw
        std::optional<std::string> deviceKind;   // pairRequest.deviceKind, raw; absent means an old iPhone app
        std::optional<int> protocolVersion;      // pairRequest.protocolVersion
        uint64_t nowUnixMs = 0;
    };
    enum class AddResult { Added, AlreadyPaired, TooManyDevices, Invalid };
    // A new pairing. Removes the hash from the revoked list. AlreadyPaired leaves the record's name and
    // role as they are (Re-pair is the way to keep a role, D1) and only records protocolVersion 3.
    AddResult addPairedDevice(const NewDevice& d, const DeviceRecord** out = nullptr);

    enum class RePairResult { Replaced, NotFound, HashInUse, Invalid };
    // Re-pair on a device's row: the record gets the new hash (and deviceId), pairedAtUnixMs = now and
    // the new kind/protocol version if given; its name and role stay. The new hash leaves the revoked list.
    RePairResult rePair(const std::string& deviceId, const std::vector<uint8_t>& newCertSHA256, uint64_t nowUnixMs,
                        std::optional<int> protocolVersion = std::nullopt,
                        std::optional<std::string> deviceKind = std::nullopt);

    struct RevokeResult {
        bool found = false;
        bool nowEmpty = false; // no device left: the caller rotates serviceId before save() (v3 §3.2)
        std::optional<DeviceRecord> removed;
    };
    RevokeResult revoke(const std::string& deviceId);
    RevokeResult revokeByHash(const std::vector<uint8_t>& certSHA256);
    // Every device goes to the revoked list. Returns how many were removed.
    size_t revokeAll();

    // False when the device is unknown or the name is empty after sanitising.
    bool rename(const std::string& deviceId, const std::string& newName);
    // False when the device is unknown or role is Unknown.
    bool setRole(const std::string& deviceId, PetRole role);

    // On connect and disconnect: last seen and the session's protocol version. Memory only; the caller
    // saves at most every 10 minutes and on exit (saveDeviceListOnly). False when the hash is unknown.
    bool touch(const std::vector<uint8_t>& certSHA256, uint64_t nowUnixMs,
               std::optional<int> protocolVersion = std::nullopt);
    bool hasUnsavedTouches() const { return touched_; }

    // A copy of this store that was saved (save() or saveDeviceListOnly() on the copy, outside the
    // caller's lock): takes over what pairing.json now holds, and clears the unsaved-touch flag unless a
    // touch came after the copy was taken.
    void adoptSave(const DeviceStore& saved);

    // ---- saving --------------------------------------------------------------------------------

    // The save sequence (pet-camera.md §5): devices.json first, with written = {pairing.serviceId,
    // mirror()} and before = what pairing.json holds now; then pairing.json with the mirror in
    // pinnedClientCertSHA256. Both atomic and owner-only. Sets pairing.pinnedClientCertSHA256.
    // Every serviceId or pin change must go through here, never through saveHostStateAtomically alone.
    bool save(HostPersistedState& pairing, std::string* err = nullptr);

    // devices.json only, for lastSeen flushes. Refused when the mirror changed since the last save
    // (a pairing, revoke or role change needs save()). It also sets before = written, so call it on
    // every clean exit: otherwise, right after the first owner is paired, an older build clearing the
    // pin without a serviceId change would match "before" and read as an interrupted save (row 3).
    bool saveDeviceListOnly(std::string* err = nullptr);

    // What this store believes pairing.json holds (after a load or save).
    const PairingSnapshot& pairingOnDisk() const { return onDisk_; }

    const std::wstring& stateDir() const { return dir_; }
    std::wstring devicesPath() const { return dir_ + L"\\devices.json"; }

    // Test only: when set, save() stops right after devices.json is written, as a crash would.
    void setCrashBetweenWritesForTest(bool on) { crashBetweenWrites_ = on; }

private:
    std::wstring dir_;
    std::vector<DeviceRecord> devices_;
    std::vector<std::vector<uint8_t>> revoked_;
    PairingSnapshot onDisk_;
    bool touched_ = false;
    uint64_t touchSeq_ = 0; // counts touches, so adoptSave can tell a later one
    bool crashBetweenWrites_ = false;

    DeviceRecord* findMutable(const std::string& deviceId);
    void addToRevoked(const std::vector<uint8_t>& h);
    void removeFromRevoked(const std::vector<uint8_t>& h);
    void addMigratedOwner(const std::vector<uint8_t>& h, uint64_t nowUnixMs);
    std::string toJson(const PairingSnapshot& written, const PairingSnapshot& before) const;
    bool writeDevicesFile(const PairingSnapshot& written, std::string* err);
};

} // namespace kvhost
