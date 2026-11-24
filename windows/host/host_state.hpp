// Host pairing state (port of HostPersistedState in HostStreamServer.swift), stored as
// %USERPROFILE%\kinect-viewer\.host-state\pairing.json with an owner-only NTFS ACL.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#ifdef KV_TESTING
#include <functional>
#endif

namespace kvhost {

struct HostPersistedState {
    // serverIdentity's private key, a 32-byte P-256 scalar. In memory it is always the plain key: a load
    // unwraps it. On disk it is kept wrapped (privateKeyWrapped, below); a file written before the wrap holds
    // it plain as privateKeyRaw until the first start migrates it (migrateHostKeyToWrapped).
    std::vector<uint8_t> privateKeyRaw;
    // serverIdentity.privateKeyWrapped as read from disk (empty in a file that holds the key plain only). A
    // save makes a new one from privateKeyRaw and ignores this one.
    std::vector<uint8_t> privateKeyWrapped;
    std::vector<uint8_t> certificateDER; // serverIdentity.certificateDER
    std::string serviceId;
    std::optional<std::vector<uint8_t>> pinnedClientCertSHA256;
    bool allowPhoneModeSwitch = true;
    // Phase 7: power control has its own setting, on by default, apart from mode switching.
    bool allowPhoneSensorPowerControl = true;
    // "Turn the Kinect off when the iPhone disconnects", off by default.
    bool turnOffKinectOnPhoneDisconnect = false;
};

std::string toJson(const HostPersistedState& s);
bool fromJson(const std::string& json, HostPersistedState& s);

std::wstring defaultHostStateDir(); // %USERPROFILE%\kinect-viewer\.host-state

// A new identity's certificate common name: 16 random hex characters, so an internet scan can't tell these hosts by
// a fixed name. Devices pin the certificate's hash and never read the name. An existing identity keeps its
// certificate (and so its old "kv2-host" name): changing it would change the hash every device pinned.
std::string newHostCommonName();

// The host writes this file into its state folder when it creates or first uses it, so a --state-dir
// check can tell the host's folder from one that merely holds a file called devices.json. A folder from
// before the marker is known by a pairing.json that reads as the host's (fromJson above).
constexpr const wchar_t* kHostStateMarkerFile = L".kinect-viewer-host-state";

// Creates the directory (and parents) and applies a protected DACL granting only the current user.
bool ensureOwnerOnlyDirectory(const std::wstring& dir, std::string* err = nullptr);
bool applyOwnerOnlyAcl(const std::wstring& path, bool is_directory, std::string* err = nullptr);
// True when the DACL is protected and every ACE grants the current user only.
bool isOwnerOnly(const std::wstring& path);

// What loading pairing.json (this PC's identity) found. Missing: there is none, a first run. Unusable: it is
// there, or whether it is there couldn't be checked, but it couldn't be read or parsed, or its wrapped key
// couldn't be unwrapped (a file from another Windows install, or a damaged one); err says which, with the
// Win32 error. A start never makes a new identity over an Unusable one. Loaded: s.privateKeyRaw is the plain
// key, unwrapped when the file holds it wrapped only.
// passing: set when it is Unusable because the wrapped key wouldn't unwrap for a reason that may pass (out of
// memory, the DPAPI service not answering: anything but a data error, hostKeyUnwrapErrorIsData). The error then
// says to try again, not to reset: a reset makes every device pair again, and a passing fault isn't worth that.
enum class HostStateLoad { Loaded, Missing, Unusable };
HostStateLoad loadHostStateChecked(const std::wstring& dir, HostPersistedState& s, std::string* err = nullptr,
                                   bool* passing = nullptr);
// A CryptUnprotectData error that says the wrapped key itself is no good here: damaged, wrapped with other entropy,
// truncated, or wrapped on another PC or Windows install whose master key this PC hasn't got (ERROR_INVALID_DATA,
// ERROR_INVALID_PARAMETER, NTE_BAD_DATA, NTE_BAD_KEY, NTE_BAD_KEY_STATE). Only then is a reset the way out.
bool hostKeyUnwrapErrorIsData(unsigned long win32);
#ifdef KV_TESTING
// Test only, and only where KV_TESTING is defined: a test program that compiles its own copy of host_state.cpp
// with it (windows/CMakeLists.txt). kv_host, which kinect-viewer and kinect-cli link, is built without it, so
// neither the switches nor their checks are in a shipping program, and a shipping source that names them doesn't
// compile. A row in test_protocol_golden.cpp pins both.
// Make wrapping the host key, or unwrapping it, fail with this Win32 error (0: as DPAPI answers).
void setHostKeyDpapiErrorsForTest(unsigned long wrap_error, unsigned long unwrap_error);
// Run hook between a plain save's check and its write (saveHostStateAtomically with DPAPI not working), as
// another start of the host on the same folder could act there. An empty hook clears it.
void setHostKeyBeforePlainSaveForTest(std::function<void()> hook);
// Make the nth_call-th pairing.json replace from now on (1: the next) fail with win32_error, leaving the files as
// Windows can midway: ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 with a backup name moves the replaced file to it;
// ERROR_UNABLE_TO_MOVE_REPLACEMENT without one removes the replaced file. 0 disarms it.
void setHostKeyReplaceFaultForTest(int nth_call, unsigned long win32_error);
#endif
// At a start, before pairing.json is loaded: what an interrupted write left in dir. A pairing.json.prev with no
// pairing.json (a plain save that couldn't finish) goes back as pairing.json, so the folder is never read as a first
// run, which would make a new identity and lose every paired device; beside a pairing.json it is removed (it may hold
// the key plain). Temp files of writes that never finished are removed too: pairing.json.tmp and the
// pairing.json.tmp.<pid> of a process that is gone. Returns what it did, for the reconcile log (empty: nothing).
std::string recoverPairingLeftovers(const std::wstring& dir);
// loadHostStateChecked(...) == Loaded.
bool loadHostState(const std::wstring& dir, HostPersistedState& s);
// Keeps <dir>\pairing.json as pairing.json.bad (keepBadCopy: numbered, older copies kept). what: what happened,
// as a phrase for an error text ("kept as pairing.json.bad" or "keeping a copy ... failed (Win32 error n)").
bool keepPairingJsonAsBad(const std::wstring& dir, std::string* what = nullptr);
// The owner's explicit reset of this PC's identity, the only way to a new one (a start never calls it).
// It loads pairing.json again first: the "unusable" the owner saw may be stale (another program let go of the
// file, or a human put a good copy back). One that loads now is NotNeeded: nothing is written, and the caller
// just starts with it. Otherwise it keeps the current pairing.json as a .bad copy (keepBadCopy) and writes a new
// identity with a new serviceId and a first run's settings, atomically and owner-only. A copy that fails while
// the file can be read is Refused (err): the old identity is never written over without a copy. One that can't
// be read can't be copied either; the reset goes ahead (the owner asked) and note says so. Every paired device
// must pair again: the new serviceId makes the next load of the device list drop it (DeviceStore, row 4a).
// pet-camera.json is untouched. Failed (err) when the new pairing.json couldn't be made, written or read back. err
// says whether pairing.json was replaced: when the write itself failed the old one is as it was, and when only the
// read-back failed the new file is there (the old one is kept as its .bad copy). note: what happened to the old file.
enum class IdentityReset { Done, NotNeeded, Refused, Failed };
IdentityReset resetHostIdentity(const std::wstring& dir, std::string* err, std::string* note = nullptr);
// Writes pairing.json.tmp (owner-only) then atomically replaces pairing.json, with the private key wrapped
// (DPAPI, this PC's machine scope and this app's entropy). The new wrap is unwrapped and compared before anything
// is written, and the file is read back and unwrapped after: false (err) when that fails. When the key can't be
// wrapped (DPAPI not working) and pairing.json is missing or still holds the key plain, the save writes it plain,
// as a build before the wrap did, so pairing a phone and saving a setting still work (reachability first); the
// next start's migration wraps it. Over a file that holds it wrapped, it never writes the key plain: the save
// fails instead.
bool saveHostStateAtomically(const std::wstring& dir, const HostPersistedState& s, std::string* err = nullptr);

// The host key at rest. A pairing.json from a build before the wrap holds the key plain; the first start
// migrates it, after a load that worked:
//   1. the key is wrapped, and the wrap unwrapped and compared in memory;
//   2. pairing.json is written with both, the plain key and the wrapped one;
//   3. that file is read back from disk and its wrapped key unwrapped and compared with its plain one;
//   4. only then is pairing.json written with the wrapped key alone, and read back and checked again.
// Any step that fails leaves a file that still holds the plain key (KeptPlain, note says why): the host
// starts with it as before, and the next start tries again. A file already wrapped is AlreadyWrapped:
// nothing is written, so running it again changes nothing. Unreadable: pairing.json couldn't be read or
// parsed (the start's own load says why). read_back_for_test stands in for the unwrap in steps 3 and 4.
enum class HostKeyMigration { AlreadyWrapped, Migrated, KeptPlain, Unreadable };
HostKeyMigration migrateHostKeyToWrapped(const std::wstring& dir, std::string* note = nullptr,
                                         bool (*read_back_for_test)(const std::vector<uint8_t>& wrapped,
                                                                    std::vector<uint8_t>& key) = nullptr);

} // namespace kvhost
