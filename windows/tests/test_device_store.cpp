// The device list (devices.json), its reconciliation with pairing.json and the downgrade mirror
// (pet-camera.md §5, protocol v3 §2.1/§3.2/§4.4), plus pet-camera.json and the sharing schedule.
// Synthetic data in temp folders only: no real state directory is read or written.
#include "kv_test.hpp"

#include "device_store.hpp"
#include "host_state.hpp"
#include "json_lite.hpp"
#include "outside_address.hpp" // AddressPolicy
#include "pet_settings.hpp"
#include "state_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

using namespace kvhost;
using Bytes = std::vector<uint8_t>;

namespace {

std::wstring freshDir(const wchar_t* tag) {
    static std::atomic<int> counter{0};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-devices-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(++counter);
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// A temp state directory that is removed when the test ends.
struct TempState {
    std::wstring dir;
    explicit TempState(const wchar_t* tag) : dir(freshDir(tag)) { ensureOwnerOnlyDirectory(dir); }
    ~TempState() { removeTree(dir); }
};

Bytes hashOf(uint8_t n) {
    Bytes h(32);
    for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(n + i * 7);
    return h;
}

HostPersistedState pairingState(const std::string& serviceId, std::optional<Bytes> pin) {
    HostPersistedState s;
    s.privateKeyRaw = Bytes(32, 0x11); // synthetic bytes; nothing here does crypto
    s.certificateDER = Bytes(40, 0x22);
    s.serviceId = serviceId;
    s.pinnedClientCertSHA256 = std::move(pin);
    return s;
}

// What pairing.json holds now, as the next start would load it.
HostPersistedState onDisk(const std::wstring& dir) {
    HostPersistedState s;
    KV_CHECK(loadHostState(dir, s));
    return s;
}

// A process start: load pairing.json and reconcile a fresh store against it.
DeviceStore::ReconcileResult restart(const std::wstring& dir, DeviceStore& store, HostPersistedState& pairing,
                                     uint64_t now = 5000) {
    pairing = onDisk(dir);
    store = DeviceStore(dir);
    return store.loadAndReconcile(pairing, now);
}

DeviceStore::NewDevice newDevice(const Bytes& h, PetRole role, uint64_t now,
                                 std::optional<std::string> name = std::nullopt) {
    DeviceStore::NewDevice d;
    d.certSHA256 = h;
    d.role = role;
    d.nowUnixMs = now;
    d.deviceName = std::move(name);
    return d;
}

bool writeText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    return ok && n == text.size();
}

std::string b64(const Bytes& b) { return base64Encode(b.data(), b.size()); }

// A store with owner A (paired at 100) and viewer B (at 200), saved under serviceId "S1".
void setUpTwo(const std::wstring& dir, DeviceStore& store, HostPersistedState& pairing) {
    pairing = pairingState("S1", std::nullopt);
    KV_REQUIRE(saveHostStateAtomically(dir, pairing));
    store = DeviceStore(dir);
    store.loadAndReconcile(pairing, 50);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(1), PetRole::Owner, 100, "Owner's iPhone")) ==
             DeviceStore::AddResult::Added);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 200, "Second phone")) ==
             DeviceStore::AddResult::Added);
    KV_CHECK(store.save(pairing));
}

} // namespace

// ---- migration (row 1) -----------------------------------------------------------------------

KV_TEST(migration_pin_becomes_owner_iphone) {
    TempState t(L"migrate");
    HostPersistedState p = pairingState("S1", hashOf(9));
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    DeviceStore store(t.dir);
    const auto r = store.loadAndReconcile(p, 1234);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::Migrated);
    KV_CHECK(!r.log.empty());
    KV_REQUIRE(store.size() == 1);
    const DeviceRecord& d = store.devices()[0];
    KV_CHECK(d.certSHA256 == hashOf(9));
    KV_CHECK(d.name == "iPhone");
    KV_CHECK(d.role == PetRole::Owner);
    KV_CHECK(d.kind == DeviceKind::IPhone);
    KV_CHECK(d.pairedAtUnixMs == 1234);
    KV_CHECK(d.deviceId.size() == 16);
    KV_CHECK(d.deviceId == deviceIdFromHash(hashOf(9)));
    KV_CHECK(d.deviceId.find_first_not_of("0123456789abcdef") == std::string::npos);
    KV_CHECK(!d.lastSeenUnixMs && !d.lastProtocolVersion);
    KV_CHECK(fileExists(t.dir + L"\\devices.json"));
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(9));

    // The next start is consistent and changes nothing.
    DeviceStore again(t.dir);
    HostPersistedState p2;
    const auto r2 = restart(t.dir, again, p2);
    KV_CHECK(r2.which == DeviceStore::ReconcileCase::Consistent);
    KV_CHECK(!r2.pairingRewritten && !r2.devicesRewritten);
    KV_CHECK(again.size() == 1 && again.devices()[0].name == "iPhone" && again.devices()[0].pairedAtUnixMs == 1234);
    KV_CHECK(p2.pinnedClientCertSHA256 == hashOf(9));
}

KV_TEST(migration_without_a_pin_starts_empty) {
    TempState t(L"migrate-empty");
    HostPersistedState p = pairingState("S1", std::nullopt);
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    DeviceStore store(t.dir);
    const auto r = store.loadAndReconcile(p, 1);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::Migrated);
    KV_CHECK(store.empty());
    KV_CHECK(!store.mirror());
    KV_CHECK(fileExists(t.dir + L"\\devices.json"));
    DeviceStore again(t.dir);
    HostPersistedState p2;
    KV_CHECK(restart(t.dir, again, p2).which == DeviceStore::ReconcileCase::Consistent);
}

// ---- the three crash cases (row 3) --------------------------------------------------------

KV_TEST(crash_revoking_the_last_device_rotates_service_id) {
    TempState t(L"crash-last");
    HostPersistedState p = pairingState("S1", hashOf(1));
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    DeviceStore store(t.dir);
    store.loadAndReconcile(p, 10); // the pin becomes owner "iPhone"
    const auto rv = store.revoke(deviceIdFromHash(hashOf(1)));
    KV_CHECK(rv.found && rv.nowEmpty);
    p.serviceId = "S2"; // the caller rotates serviceId when the list is empty (v3 §3.2)
    store.setCrashBetweenWritesForTest(true);
    KV_CHECK(!store.save(p));
    // pairing.json still holds the old pin and serviceId.
    KV_CHECK(onDisk(t.dir).serviceId == "S1" && onDisk(t.dir).pinnedClientCertSHA256 == hashOf(1));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::RepairedPairing);
    KV_CHECK(r.pairingRewritten);
    KV_CHECK(s2.empty());
    KV_CHECK(!s2.findByHash(hashOf(1)));
    KV_CHECK(s2.isRevoked(hashOf(1)));
    KV_CHECK(p2.serviceId == "S2" && !p2.pinnedClientCertSHA256);
    KV_CHECK(onDisk(t.dir).serviceId == "S2" && !onDisk(t.dir).pinnedClientCertSHA256);
    DeviceStore s3(t.dir);
    KV_CHECK(restart(t.dir, s3, p2).which == DeviceStore::ReconcileCase::Consistent);
}

KV_TEST(crash_revoking_all_devices) {
    TempState t(L"crash-all");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    KV_CHECK(store.revokeAll() == 2);
    p.serviceId = "S2";
    store.setCrashBetweenWritesForTest(true);
    KV_CHECK(!store.save(p));
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(1));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::RepairedPairing);
    KV_CHECK(s2.empty());
    KV_CHECK(s2.isRevoked(hashOf(1)) && s2.isRevoked(hashOf(2)));
    KV_CHECK(onDisk(t.dir).serviceId == "S2" && !onDisk(t.dir).pinnedClientCertSHA256);
}

KV_TEST(crash_pairing_the_first_owner) {
    TempState t(L"crash-first");
    HostPersistedState p = pairingState("S1", std::nullopt);
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    DeviceStore store(t.dir);
    store.loadAndReconcile(p, 10);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(3), PetRole::Owner, 20)) == DeviceStore::AddResult::Added);
    store.setCrashBetweenWritesForTest(true);
    KV_CHECK(!store.save(p));
    KV_CHECK(!onDisk(t.dir).pinnedClientCertSHA256);

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::RepairedPairing);
    KV_REQUIRE(s2.size() == 1);
    KV_CHECK(s2.findByHash(hashOf(3)) && s2.findByHash(hashOf(3))->role == PetRole::Owner);
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(3) && onDisk(t.dir).serviceId == "S1");
}

KV_TEST(crash_pairing_a_second_owner_drops_no_listed_device) {
    TempState t(L"crash-second");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(4), PetRole::Owner, 300)) == DeviceStore::AddResult::Added);
    store.setCrashBetweenWritesForTest(true);
    KV_CHECK(!store.save(p));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::RepairedPairing);
    KV_CHECK(s2.size() == 3);
    KV_CHECK(s2.findByHash(hashOf(1)) && s2.findByHash(hashOf(2)) && s2.findByHash(hashOf(4)));
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(4));
}

KV_TEST(crash_revoking_an_owner_does_not_readmit_it) {
    TempState t(L"crash-owner");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    const auto rv = store.revoke(deviceIdFromHash(hashOf(1)));
    KV_CHECK(rv.found && !rv.nowEmpty);
    store.setCrashBetweenWritesForTest(true);
    KV_CHECK(!store.save(p));
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(1)); // the stale mirror

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::RepairedPairing);
    KV_CHECK(!s2.findByHash(hashOf(1)));
    KV_CHECK(s2.findByHash(hashOf(2))); // the viewer stays, and the list is not empty
    KV_CHECK(!onDisk(t.dir).pinnedClientCertSHA256); // no owner left: no mirror key
    KV_CHECK(onDisk(t.dir).serviceId == "S1");
}

// ---- downgrades (rows 4a-4c) ------------------------------------------------------------------

KV_TEST(downgrade_unpair_then_upgrade_leaves_the_device_refused) {
    TempState t(L"down-unpair");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    // An older build's "Unpair": clears the pin and rotates serviceId.
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256.reset();
    old.serviceId = "S-old";
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradeUnpair);
    KV_CHECK(!r.log.empty());
    KV_CHECK(s2.empty());
    KV_CHECK(!s2.findByHash(hashOf(1)) && !s2.findByHash(hashOf(2)));
    KV_CHECK(onDisk(t.dir).serviceId == "S-old" && !onDisk(t.dir).pinnedClientCertSHA256);
    DeviceStore s3(t.dir);
    KV_CHECK(restart(t.dir, s3, p2).which == DeviceStore::ReconcileCase::Consistent);
}

KV_TEST(downgrade_unpair_and_repair_admits_the_new_pin_as_owner) {
    TempState t(L"down-repair");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    // The older build unpaired, then paired phone C.
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256 = hashOf(7);
    old.serviceId = "S-old";
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2, 777);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradeUnpair);
    KV_REQUIRE(s2.size() == 1);
    KV_CHECK(s2.devices()[0].certSHA256 == hashOf(7));
    KV_CHECK(s2.devices()[0].name == "iPhone" && s2.devices()[0].role == PetRole::Owner);
    KV_CHECK(s2.devices()[0].pairedAtUnixMs == 777);
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(7));
}

KV_TEST(downgrade_unpair_with_a_revoked_pin_admits_nothing) {
    TempState t(L"down-revoked");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    store.revoke(deviceIdFromHash(hashOf(1)));
    KV_CHECK(store.save(p));
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256 = hashOf(1); // the revoked phone, paired again on the old build
    old.serviceId = "S-old";
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradeUnpair);
    KV_CHECK(s2.empty());
    KV_CHECK(!s2.findByHash(hashOf(1)) && s2.isRevoked(hashOf(1)));
    KV_CHECK(!onDisk(t.dir).pinnedClientCertSHA256);
}

KV_TEST(downgrade_pair_replaces_the_mirror_device) {
    TempState t(L"down-pair");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    // An older build paired phone C (it replaces the pin, same serviceId).
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256 = hashOf(8);
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2, 900);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradePair);
    KV_CHECK(s2.size() == 2);
    KV_CHECK(!s2.findByHash(hashOf(1)));                // the mirror device was dropped
    KV_CHECK(s2.findByHash(hashOf(2)));                 // the viewer stays
    const DeviceRecord* c = s2.findByHash(hashOf(8));
    KV_REQUIRE(c != nullptr);
    KV_CHECK(c->role == PetRole::Owner && c->name == "iPhone" && c->kind == DeviceKind::IPhone);
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(8) && onDisk(t.dir).serviceId == "S1");
}

KV_TEST(downgrade_pair_of_a_revoked_or_listed_pin_changes_nothing) {
    TempState t(L"down-pair-ignored");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(5), PetRole::Viewer, 250)) == DeviceStore::AddResult::Added);
    store.revoke(deviceIdFromHash(hashOf(5)));
    KV_CHECK(store.save(p));

    // The old build pinned a revoked hash: ignored, the mirror is written back.
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256 = hashOf(5);
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));
    DeviceStore s2(t.dir);
    HostPersistedState p2;
    auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradePair);
    KV_CHECK(s2.size() == 2 && !s2.findByHash(hashOf(5)) && s2.findByHash(hashOf(1)));
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(1));

    // The old build pinned the listed viewer: nothing changes, the mirror is written back.
    old = onDisk(t.dir);
    old.pinnedClientCertSHA256 = hashOf(2);
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));
    DeviceStore s3(t.dir);
    r = restart(t.dir, s3, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradePair);
    KV_CHECK(s3.size() == 2 && s3.findByHash(hashOf(1)) && s3.findByHash(hashOf(2)));
    KV_CHECK(s3.findByHash(hashOf(2))->role == PetRole::Viewer);
    KV_CHECK(onDisk(t.dir).pinnedClientCertSHA256 == hashOf(1));
}

KV_TEST(downgrade_clear_drops_every_device) {
    TempState t(L"down-clear");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    // The exit flush makes before == written. Without it, "before" is still the state before the first
    // owner was paired (no pin), and a cleared pin reads as row 3, an interrupted save.
    KV_CHECK(store.saveDeviceListOnly());
    HostPersistedState old = onDisk(t.dir);
    old.pinnedClientCertSHA256.reset(); // cleared without a serviceId change
    KV_REQUIRE(saveHostStateAtomically(t.dir, old));

    DeviceStore s2(t.dir);
    HostPersistedState p2;
    const auto r = restart(t.dir, s2, p2);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::DowngradeClear);
    KV_CHECK(s2.empty());
    KV_CHECK(!onDisk(t.dir).pinnedClientCertSHA256 && onDisk(t.dir).serviceId == "S1");
}

// ---- pairing, revoke and the lists --------------------------------------------------------

KV_TEST(revoked_hash_is_never_readmitted_by_reconciliation_only_by_pairing) {
    TempState t(L"revoked");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    store.revoke(deviceIdFromHash(hashOf(2)));
    KV_CHECK(store.save(p));
    KV_CHECK(store.isRevoked(hashOf(2)) && !store.findByHash(hashOf(2)));
    // Pairing it again on the PC removes it from the revoked list.
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 400)) == DeviceStore::AddResult::Added);
    KV_CHECK(!store.isRevoked(hashOf(2)) && store.findByHash(hashOf(2)));
    KV_CHECK(store.save(p));
    DeviceStore s2(t.dir);
    HostPersistedState p2;
    KV_CHECK(restart(t.dir, s2, p2).which == DeviceStore::ReconcileCase::Consistent);
    KV_CHECK(s2.findByHash(hashOf(2)) && !s2.isRevoked(hashOf(2)));
}

KV_TEST(revoked_list_is_capped_oldest_first) {
    TempState t(L"revoked-cap");
    DeviceStore store(t.dir);
    for (int i = 0; i < 300; ++i) {
        Bytes h = hashOf(0);
        h[0] = static_cast<uint8_t>(i & 0xFF);
        h[1] = static_cast<uint8_t>(i >> 8);
        KV_CHECK(store.addPairedDevice(newDevice(h, PetRole::Viewer, i)) == DeviceStore::AddResult::Added);
        KV_CHECK(store.revokeByHash(h).found);
    }
    KV_CHECK(store.revoked().size() == kMaxRevokedHashes);
    KV_CHECK(store.revoked().front()[0] == 44 && store.revoked().front()[1] == 0); // 0..43 dropped
    KV_CHECK(store.revoked().back()[0] == (299 & 0xFF) && store.revoked().back()[1] == 1);
}

KV_TEST(sixteen_device_limit) {
    TempState t(L"limit");
    DeviceStore store(t.dir);
    for (uint8_t i = 0; i < 16; ++i) {
        KV_CHECK(store.addPairedDevice(newDevice(hashOf(i), PetRole::Viewer, i)) == DeviceStore::AddResult::Added);
    }
    KV_CHECK(store.full());
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(100), PetRole::Viewer, 1)) ==
             DeviceStore::AddResult::TooManyDevices);
    KV_CHECK(store.size() == 16);
    // A listed device pairing again is not a 17th device.
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(3), PetRole::Owner, 1)) == DeviceStore::AddResult::AlreadyPaired);
    KV_CHECK(store.findByHash(hashOf(3))->role == PetRole::Viewer); // the role is kept (Re-pair keeps it, D1)
    store.revoke(deviceIdFromHash(hashOf(0)));
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(100), PetRole::Viewer, 1)) == DeviceStore::AddResult::Added);
    KV_CHECK(store.addPairedDevice(newDevice(Bytes(31, 1), PetRole::Viewer, 1)) == DeviceStore::AddResult::Invalid);
    KV_CHECK(store.addPairedDevice(newDevice(hashOf(200), PetRole::Unknown, 1)) == DeviceStore::AddResult::Invalid);
}

KV_TEST(default_names_fill_the_lowest_free_number) {
    TempState t(L"names");
    DeviceStore store(t.dir);
    for (uint8_t i = 1; i <= 3; ++i) store.addPairedDevice(newDevice(hashOf(i), PetRole::Viewer, i));
    KV_CHECK(store.findByHash(hashOf(1))->name == "iPhone");
    KV_CHECK(store.findByHash(hashOf(2))->name == "iPhone 2");
    KV_CHECK(store.findByHash(hashOf(3))->name == "iPhone 3");
    store.revoke(deviceIdFromHash(hashOf(2)));
    KV_CHECK(store.nextDefaultName() == "iPhone 2");
    // A name that sanitises to nothing gets the default too.
    store.addPairedDevice(newDevice(hashOf(4), PetRole::Viewer, 4, std::string(" \t\r\n ")));
    KV_CHECK(store.findByHash(hashOf(4))->name == "iPhone 2");
    store.addPairedDevice(newDevice(hashOf(5), PetRole::Viewer, 5, std::string("Kitchen iPad")));
    KV_CHECK(store.findByHash(hashOf(5))->name == "Kitchen iPad");
    KV_CHECK(nextDefaultDeviceName({}) == "iPhone");
    KV_CHECK(nextDefaultDeviceName({"iPhone", "iPhone 3"}) == "iPhone 2");
}

// A newly paired device can't take a name already on the list: it gets the next free "<name> N". A rename on the
// PC is the PC user's choice and is kept as typed.
KV_TEST(a_paired_device_never_takes_a_name_already_listed) {
    TempState t(L"unique-names");
    DeviceStore store(t.dir);
    store.addPairedDevice(newDevice(hashOf(1), PetRole::Owner, 1, std::string("Owner's iPhone")));
    store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 2, std::string("Owner's iPhone")));
    store.addPairedDevice(newDevice(hashOf(3), PetRole::Viewer, 3, std::string("  Owner's\tiPhone ")));
    KV_CHECK(store.findByHash(hashOf(1))->name == "Owner's iPhone");
    KV_CHECK(store.findByHash(hashOf(2))->name == "Owner's iPhone 2");
    KV_CHECK(store.findByHash(hashOf(3))->name == "Owner's iPhone 3");
    // A bidi control hidden in the name doesn't make it a different one.
    store.addPairedDevice(newDevice(hashOf(4), PetRole::Viewer, 4, std::string("Owner's\xe2\x80\x8f iPhone")));
    KV_CHECK(store.findByHash(hashOf(4))->name == "Owner's iPhone 4");
    // Nor does a character that shows nothing: the zero-width space, the word joiner, the soft hyphen, a zero-width
    // no-break space mid-name, an invisible operator.
    const std::pair<const char*, const char*> hidden[] = {
        {"Owner's iPhone\xe2\x80\x8b", "U+200B"}, {"Owner's\xe2\x81\xa0 iPhone", "U+2060"},
        {"Own\xc2\xad" "er's iPhone", "U+00AD"},   {"Owner's iP\xef\xbb\xbf" "hone", "U+FEFF"},
        {"Owner's i\xe2\x81\xa3" "Phone", "U+2063"},
    };
    int n = 9;
    for (const auto& h : hidden) {
        store.addPairedDevice(newDevice(hashOf(static_cast<uint8_t>(n)), PetRole::Viewer, static_cast<uint64_t>(n), std::string(h.first)));
        const std::string name = store.findByHash(hashOf(static_cast<uint8_t>(n)))->name;
        KV_CHECK_MSG(name.rfind("Owner's iPhone ", 0) == 0 && name != "Owner's iPhone", std::string(h.second) + ": " + name);
        ++n;
    }
    // The default name's numbering is the same rule.
    store.addPairedDevice(newDevice(hashOf(5), PetRole::Viewer, 5, std::string("iPhone")));
    store.addPairedDevice(newDevice(hashOf(6), PetRole::Viewer, 6));
    KV_CHECK(store.findByHash(hashOf(5))->name == "iPhone");
    KV_CHECK(store.findByHash(hashOf(6))->name == "iPhone 2");
    // Within 64 bytes: the name is cut at a character boundary to make room for the number.
    const std::string longName = std::string(62, 'a') + "\xc3\xa9"; // 64 bytes
    store.addPairedDevice(newDevice(hashOf(7), PetRole::Viewer, 7, longName));
    store.addPairedDevice(newDevice(hashOf(8), PetRole::Viewer, 8, longName));
    KV_CHECK(store.findByHash(hashOf(7))->name == longName);
    KV_CHECK(store.findByHash(hashOf(8))->name == std::string(62, 'a') + " 2");
    KV_CHECK(uniqueDeviceName("x", {"x", "x 2"}) == "x 3");
    // A rename keeps what the PC user typed.
    KV_CHECK(store.rename(deviceIdFromHash(hashOf(2)), "Owner's iPhone"));
    KV_CHECK(store.findByHash(hashOf(2))->name == "Owner's iPhone");
}

KV_TEST(device_name_sanitising) {
    KV_CHECK(sanitizeDeviceName("  Owner's iPhone  ") == std::optional<std::string>("Owner's iPhone"));
    KV_CHECK(sanitizeDeviceName("\tA\x01" "B\nC\x7f" "D\r") == std::optional<std::string>("A B C D"));
    KV_CHECK(sanitizeDeviceName("A\xc2\x85" "B") == std::optional<std::string>("A B")); // C1 NEL
    KV_CHECK(sanitizeDeviceName("A\xe2\x80\xa8" "B") == std::optional<std::string>("A B")); // U+2028
    KV_CHECK(!sanitizeDeviceName(""));
    KV_CHECK(!sanitizeDeviceName("   "));
    KV_CHECK(!sanitizeDeviceName("\x01\x02\x03"));
    KV_CHECK(!sanitizeDeviceName("\xe3\x80\x80")); // only an ideographic space
    // Invalid UTF-8 becomes U+FFFD rather than passing bad bytes on.
    KV_CHECK(sanitizeDeviceName("a\xff" "b") == std::optional<std::string>("a\xef\xbf\xbd" "b"));
    KV_CHECK(sanitizeDeviceName("\xc0\xaf") == std::optional<std::string>("\xef\xbf\xbd\xef\xbf\xbd")); // overlong
    // Exactly 64 bytes is kept; 65 is cut.
    KV_CHECK(sanitizeDeviceName(std::string(64, 'x'))->size() == 64);
    KV_CHECK(sanitizeDeviceName(std::string(65, 'x')) == std::optional<std::string>(std::string(64, 'x')));
    // A 3-byte character straddling byte 64 is dropped whole.
    const auto euro = sanitizeDeviceName(std::string(63, 'a') + "\xe2\x82\xac" + "zz");
    KV_REQUIRE(euro.has_value());
    KV_CHECK(*euro == std::string(63, 'a'));
    // 40 two-byte characters: cut to 32 of them (64 bytes), never mid-character.
    std::string e;
    for (int i = 0; i < 40; ++i) e += "\xc3\xa9";
    const auto cut = sanitizeDeviceName(e);
    KV_REQUIRE(cut.has_value());
    KV_CHECK(cut->size() == 64 && *cut == e.substr(0, 64));
    // The cut leaves no trailing space: 63 bytes then a space then more.
    KV_CHECK(sanitizeDeviceName(std::string(63, 'b') + " cd") == std::optional<std::string>(std::string(63, 'b')));
    // Leading controls count as spaces and are trimmed before the 64-byte limit applies.
    KV_CHECK(sanitizeDeviceName("\x01\x01" + std::string(64, 'c'))->size() == 64);
    // The bidirectional controls are dropped, so a name can't show its text out of order: a right-to-left override
    // (U+202E) that would make "iPhone" plus "gpj.exe" read as something else, the isolates (U+2066-2069), the marks
    // (U+200E, U+200F, U+061C) and the embeddings (U+202A-202D).
    KV_CHECK(sanitizeDeviceName("iPhone\xe2\x80\xae" "exe.gpj") == std::optional<std::string>("iPhoneexe.gpj"));
    KV_CHECK(sanitizeDeviceName("\xe2\x81\xa6" "A\xe2\x81\xa9" "B\xe2\x81\xa7" "C\xe2\x81\xa8") ==
             std::optional<std::string>("ABC"));
    KV_CHECK(sanitizeDeviceName("A\xe2\x80\x8e" "B\xe2\x80\x8f" "C\xd8\x9c" "D") == std::optional<std::string>("ABCD"));
    KV_CHECK(sanitizeDeviceName("\xe2\x80\xaa" "A\xe2\x80\xab" "B\xe2\x80\xac\xe2\x80\xad") ==
             std::optional<std::string>("AB"));
    KV_CHECK(!sanitizeDeviceName("\xe2\x80\xae\xe2\x80\x8f")); // nothing else: the default name
    // Characters that show nothing are dropped: U+200B, U+2060, U+00AD, U+FEFF mid-name, U+180E, U+2061-2064.
    KV_CHECK(sanitizeDeviceName("a\xe2\x80\x8b" "b\xe2\x81\xa0" "c\xc2\xad" "d\xef\xbb\xbf" "e") == std::optional<std::string>("abcde"));
    KV_CHECK(sanitizeDeviceName("a\xe1\xa0\x8e" "b\xe2\x81\xa1" "c\xe2\x81\xa4" "d") == std::optional<std::string>("abcd"));
    KV_CHECK(!sanitizeDeviceName("\xe2\x80\x8b\xc2\xad")); // nothing else: the default name
    // Joiners that emoji and scripts need are kept (U+200D, U+200C).
    KV_CHECK(sanitizeDeviceName("a\xe2\x80\x8d" "b\xe2\x80\x8c" "c") == std::optional<std::string>("a\xe2\x80\x8d" "b\xe2\x80\x8c" "c"));

    TempState t(L"rename");
    DeviceStore store(t.dir);
    store.addPairedDevice(newDevice(hashOf(1), PetRole::Viewer, 1, std::string("Old")));
    const std::string id = deviceIdFromHash(hashOf(1));
    KV_CHECK(!store.rename(id, " \n "));
    KV_CHECK(store.findByDeviceId(id)->name == "Old");
    KV_CHECK(store.rename(id, "New\tname"));
    KV_CHECK(store.findByDeviceId(id)->name == "New name");
    KV_CHECK(!store.rename("0000000000000000", "x"));
}

KV_TEST(device_kind_normalising) {
    KV_CHECK(deviceKindFromString("iphone") == DeviceKind::IPhone);
    KV_CHECK(deviceKindFromString("ipad") == DeviceKind::IPad);
    KV_CHECK(deviceKindFromString("mac") == DeviceKind::Mac);
    KV_CHECK(deviceKindFromString("other") == DeviceKind::Other);
    KV_CHECK(deviceKindFromString("watch") == DeviceKind::Other);
    KV_CHECK(deviceKindFromString("") == DeviceKind::Other);
    KV_CHECK(deviceKindFromString("iPhone") == DeviceKind::Other); // exact values only, as v3 §4.4 lists them
    KV_CHECK(std::string(deviceKindString(DeviceKind::IPad)) == "ipad");

    TempState t(L"kind");
    DeviceStore store(t.dir);
    auto d = newDevice(hashOf(1), PetRole::Viewer, 1);
    store.addPairedDevice(d); // an old phone sends no kind
    KV_CHECK(store.findByHash(hashOf(1))->kind == DeviceKind::IPhone);
    d = newDevice(hashOf(2), PetRole::Viewer, 1);
    d.deviceKind = "mac";
    d.protocolVersion = 3;
    store.addPairedDevice(d);
    KV_CHECK(store.findByHash(hashOf(2))->kind == DeviceKind::Mac);
    KV_CHECK(store.findByHash(hashOf(2))->lastProtocolVersion == 3);
    KV_CHECK(!store.findByHash(hashOf(1))->lastProtocolVersion);
    d = newDevice(hashOf(3), PetRole::Viewer, 1);
    d.deviceKind = "toaster";
    store.addPairedDevice(d);
    KV_CHECK(store.findByHash(hashOf(3))->kind == DeviceKind::Other);
}

KV_TEST(pet_role_lenient_decode) {
    KV_CHECK(petRoleFromString("owner") == PetRole::Owner);
    KV_CHECK(petRoleFromString("viewer") == PetRole::Viewer);
    KV_CHECK(petRoleFromString("admin") == PetRole::Unknown);
    KV_CHECK(petRoleFromString("Owner") == PetRole::Unknown);
    KV_CHECK(std::string(petRoleString(PetRole::Owner)) == "owner");
    KV_CHECK(std::string(petRoleString(PetRole::Viewer)) == "viewer");
}

KV_TEST(mirror_is_the_latest_paired_owner) {
    TempState t(L"mirror");
    DeviceStore store(t.dir);
    KV_CHECK(!store.mirror());
    store.addPairedDevice(newDevice(hashOf(1), PetRole::Owner, 100));
    store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 300));
    store.addPairedDevice(newDevice(hashOf(3), PetRole::Owner, 200));
    KV_CHECK(store.mirror() == hashOf(3)); // the viewer paired later doesn't count
    // Re-pairing owner 1 makes it the most recent.
    const std::string id1 = deviceIdFromHash(hashOf(1));
    KV_CHECK(store.rePair(id1, hashOf(11), 400) == DeviceStore::RePairResult::Replaced);
    KV_CHECK(store.mirror() == hashOf(11));
    // Demoting it falls back to the other owner; demoting both leaves no mirror.
    KV_CHECK(store.setRole(deviceIdFromHash(hashOf(11)), PetRole::Viewer));
    KV_CHECK(store.mirror() == hashOf(3));
    KV_CHECK(store.setRole(deviceIdFromHash(hashOf(3)), PetRole::Viewer));
    KV_CHECK(!store.mirror());
    KV_CHECK(!store.setRole(deviceIdFromHash(hashOf(3)), PetRole::Unknown));
    // Promoting the viewer paired at 300 makes it the mirror.
    KV_CHECK(store.setRole(deviceIdFromHash(hashOf(2)), PetRole::Owner));
    KV_CHECK(store.mirror() == hashOf(2));
    // The saved pairing.json carries the mirror; with no owner there is no key at all.
    HostPersistedState p = pairingState("S1", std::nullopt);
    KV_CHECK(store.save(p));
    KV_CHECK(p.pinnedClientCertSHA256 == hashOf(2) && onDisk(t.dir).pinnedClientCertSHA256 == hashOf(2));
    store.setRole(deviceIdFromHash(hashOf(2)), PetRole::Viewer);
    KV_CHECK(store.save(p));
    std::string text;
    KV_CHECK(readWholeFile(t.dir + L"\\pairing.json", text));
    KV_CHECK(text.find("pinnedClientCertSHA256") == std::string::npos);
}

KV_TEST(re_pair_keeps_name_and_role) {
    TempState t(L"repair");
    DeviceStore store(t.dir);
    auto d = newDevice(hashOf(1), PetRole::Owner, 100, std::string("Owner's iPhone"));
    d.protocolVersion = 3;
    store.addPairedDevice(d);
    store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 100));
    store.addPairedDevice(newDevice(hashOf(9), PetRole::Viewer, 100));
    store.revoke(deviceIdFromHash(hashOf(9)));
    const std::string id = deviceIdFromHash(hashOf(1));
    KV_CHECK(store.rePair(id, hashOf(2), 500) == DeviceStore::RePairResult::HashInUse);
    KV_CHECK(store.rePair("ffffffffffffffff", hashOf(5), 500) == DeviceStore::RePairResult::NotFound);
    KV_CHECK(store.rePair(id, Bytes(3, 0), 500) == DeviceStore::RePairResult::Invalid);
    // Re-pairing onto a revoked hash takes it off the revoked list.
    KV_CHECK(store.rePair(id, hashOf(9), 500, 3, std::string("ipad")) == DeviceStore::RePairResult::Replaced);
    KV_CHECK(!store.findByHash(hashOf(1)) && !store.findByDeviceId(id));
    const DeviceRecord* r = store.findByHash(hashOf(9));
    KV_REQUIRE(r != nullptr);
    KV_CHECK(r->name == "Owner's iPhone" && r->role == PetRole::Owner);
    KV_CHECK(r->deviceId == deviceIdFromHash(hashOf(9)) && r->pairedAtUnixMs == 500);
    KV_CHECK(r->kind == DeviceKind::IPad && r->lastProtocolVersion == 3);
    KV_CHECK(!store.isRevoked(hashOf(9)));
    KV_CHECK(store.size() == 2);
}

KV_TEST(touch_is_saved_with_the_device_list_only) {
    TempState t(L"touch");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    KV_CHECK(!store.hasUnsavedTouches());
    KV_CHECK(store.touch(hashOf(2), 9999, 2));
    KV_CHECK(!store.touch(hashOf(77), 9999));
    KV_CHECK(store.hasUnsavedTouches());
    KV_CHECK(store.saveDeviceListOnly());
    KV_CHECK(!store.hasUnsavedTouches());
    DeviceStore s2(t.dir);
    HostPersistedState p2;
    KV_CHECK(restart(t.dir, s2, p2).which == DeviceStore::ReconcileCase::Consistent);
    KV_CHECK(s2.findByHash(hashOf(2))->lastSeenUnixMs == 9999u);
    KV_CHECK(s2.findByHash(hashOf(2))->lastProtocolVersion == 2);
    // A change to the mirror can't be saved this way.
    s2.setRole(deviceIdFromHash(hashOf(2)), PetRole::Owner);
    s2.setRole(deviceIdFromHash(hashOf(1)), PetRole::Viewer);
    std::string err;
    KV_CHECK(!s2.saveDeviceListOnly(&err));
    KV_CHECK(!err.empty());
}

KV_TEST(revoke_reports_an_empty_list) {
    TempState t(L"revoke");
    DeviceStore store(t.dir);
    store.addPairedDevice(newDevice(hashOf(1), PetRole::Owner, 1));
    store.addPairedDevice(newDevice(hashOf(2), PetRole::Viewer, 1));
    auto r = store.revokeByHash(hashOf(1));
    KV_CHECK(r.found && !r.nowEmpty && r.removed && r.removed->certSHA256 == hashOf(1));
    r = store.revoke("0123456789abcdef");
    KV_CHECK(!r.found && !r.nowEmpty);
    r = store.revoke(deviceIdFromHash(hashOf(2)));
    KV_CHECK(r.found && r.nowEmpty);
    KV_CHECK(store.revoked().size() == 2);
}

// ---- lenient reading -------------------------------------------------------------------------

KV_TEST(lenient_loading_of_a_damaged_devices_json) {
    TempState t(L"lenient");
    HostPersistedState p = pairingState("S1", hashOf(1));
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    const std::string good = "{\"certSHA256\":\"" + b64(hashOf(1)) +
                             "\",\"deviceId\":\"wrong\",\"name\":\"\\tOwner\\u0001s\",\"role\":\"owner\","
                             "\"kind\":\"iphone\",\"pairedAtUnixMs\":10,\"lastSeenUnixMs\":20,"
                             "\"lastProtocolVersion\":3,\"futureKey\":{\"a\":[1,2]}}";
    const std::string json =
        "{\"version\":1,\"unknownTop\":true,"
        "\"written\":{\"serviceId\":\"S1\",\"mirror\":\"" + b64(hashOf(1)) + "\"},"
        "\"before\":\"not an object\","
        "\"devices\":[" + good + ","
        "{\"certSHA256\":\"" + b64(hashOf(1)) + "\",\"name\":\"duplicate\",\"role\":\"viewer\"},"
        "{\"certSHA256\":\"c2hvcnQ=\",\"name\":\"short hash\"},"
        "{\"certSHA256\":12,\"name\":\"wrong type\"},"
        "{\"name\":\"no hash\"},"
        "17,null,\"text\",[1],"
        "{\"certSHA256\":\"" + b64(hashOf(2)) + "\",\"name\":42,\"role\":\"admin\",\"kind\":\"toaster\","
        "\"pairedAtUnixMs\":-5,\"lastSeenUnixMs\":\"soon\",\"lastProtocolVersion\":1.5},"
        "{\"certSHA256\":\"" + b64(hashOf(3)) + "\",\"name\":\"on the revoked list\",\"role\":\"owner\"}"
        "],"
        "\"revoked\":[\"" + b64(hashOf(3)) + "\",\"!!\",5,\"" + b64(hashOf(3)) + "\"]}";
    KV_REQUIRE(writeText(t.dir + L"\\devices.json", json));

    DeviceStore store(t.dir);
    const auto r = store.loadAndReconcile(p, 1);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::Consistent);
    KV_REQUIRE(store.size() == 2);
    const DeviceRecord& a = store.devices()[0];
    KV_CHECK(a.certSHA256 == hashOf(1) && a.deviceId == deviceIdFromHash(hashOf(1)));
    KV_CHECK(a.name == "Owner s" && a.role == PetRole::Owner && a.kind == DeviceKind::IPhone);
    KV_CHECK(a.pairedAtUnixMs == 10 && a.lastSeenUnixMs == 20u && a.lastProtocolVersion == 3);
    const DeviceRecord& b = store.devices()[1];
    KV_CHECK(b.certSHA256 == hashOf(2));
    KV_CHECK(b.name == "iPhone"); // no usable name: the default
    KV_CHECK(b.role == PetRole::Viewer && b.kind == DeviceKind::Other);
    KV_CHECK(b.pairedAtUnixMs == 0 && !b.lastSeenUnixMs && !b.lastProtocolVersion);
    KV_CHECK(!store.findByHash(hashOf(3))); // revoked wins over a listing
    KV_CHECK(store.revoked().size() == 1 && store.isRevoked(hashOf(3)));
    // The pairing identity was not touched.
    KV_CHECK(onDisk(t.dir).certificateDER == Bytes(40, 0x22));
}

KV_TEST(unreadable_devices_json_is_kept_aside_and_rebuilt) {
    TempState t(L"garbage");
    HostPersistedState p = pairingState("S1", hashOf(4));
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    KV_REQUIRE(writeText(t.dir + L"\\devices.json", "\x01\x02 not json {{{"));
    DeviceStore store(t.dir);
    const auto r = store.loadAndReconcile(p, 1);
    KV_CHECK(r.which == DeviceStore::ReconcileCase::Migrated);
    KV_CHECK(fileExists(t.dir + L"\\devices.json.bad"));
    KV_CHECK(store.size() == 1 && store.findByHash(hashOf(4)));
    KV_CHECK(onDisk(t.dir).privateKeyRaw == Bytes(32, 0x11)); // the identity survives

    // Valid JSON that isn't an object, and an empty file, are handled the same way.
    for (const char* text : {"[1,2,3]", ""}) {
        KV_REQUIRE(writeText(t.dir + L"\\devices.json", text));
        DeviceStore s2(t.dir);
        HostPersistedState p2 = onDisk(t.dir);
        KV_CHECK(s2.loadAndReconcile(p2, 1).which == DeviceStore::ReconcileCase::Migrated);
        KV_CHECK(s2.size() == 1);
    }
}

KV_TEST(devices_json_shape) {
    TempState t(L"shape");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    std::string text;
    KV_REQUIRE(readWholeFile(t.dir + L"\\devices.json", text));
    json::Value v;
    KV_REQUIRE(json::parse(text, v));
    int version = 0;
    KV_CHECK(json::getInt(v, "version", version) && version == 1);
    const json::Value* w = v.get("written");
    const json::Value* b = v.get("before");
    KV_REQUIRE(w && b);
    std::string sid;
    Bytes m;
    KV_CHECK(json::getString(*w, "serviceId", sid) && sid == "S1");
    KV_CHECK(json::getBytes(*w, "mirror", m) && m == hashOf(1));
    KV_CHECK(json::getString(*b, "serviceId", sid) && sid == "S1");
    KV_CHECK(b->get("mirror") == nullptr); // pairing.json had no pin before this save
    const json::Value* d = v.get("devices");
    KV_REQUIRE(d && d->items.size() == 2);
    std::string s;
    KV_CHECK(json::getString(d->items[0], "deviceId", s) && s == deviceIdFromHash(hashOf(1)));
    KV_CHECK(json::getString(d->items[0], "role", s) && s == "owner");
    KV_CHECK(json::getString(d->items[1], "role", s) && s == "viewer");
    KV_CHECK(json::getString(d->items[0], "kind", s) && s == "iphone");
    KV_CHECK(json::getString(d->items[0], "name", s) && s == "Owner's iPhone");
    KV_CHECK(d->items[0].get("lastSeenUnixMs") == nullptr && d->items[0].get("lastProtocolVersion") == nullptr);
    const json::Value* rv = v.get("revoked");
    KV_CHECK(rv && rv->type == json::Value::Type::Array && rv->items.empty());
}

KV_TEST(new_files_are_owner_only) {
    TempState t(L"acl");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    PetSettings s;
    s.sharingEnabled = true;
    KV_CHECK(savePetSettingsAtomically(t.dir, s));
    KV_CHECK(isOwnerOnly(t.dir));
    KV_CHECK(isOwnerOnly(t.dir + L"\\devices.json"));
    KV_CHECK(isOwnerOnly(t.dir + L"\\pairing.json"));
    KV_CHECK(isOwnerOnly(t.dir + L"\\pet-camera.json"));
    KV_CHECK(!fileExists(t.dir + L"\\devices.json.tmp") && !fileExists(t.dir + L"\\pet-camera.json.tmp"));
}

// ---- pet-camera.json and the sharing schedule ----------------------------------------------

KV_TEST(clock_time_parsing) {
    KV_CHECK(parseClockTime("00:00") == std::optional<uint16_t>(0));
    KV_CHECK(parseClockTime("07:30") == std::optional<uint16_t>(450));
    KV_CHECK(parseClockTime("23:59") == std::optional<uint16_t>(1439));
    KV_CHECK(!parseClockTime("24:00"));
    KV_CHECK(!parseClockTime("12:60"));
    KV_CHECK(!parseClockTime("7:30"));
    KV_CHECK(!parseClockTime("07-30"));
    KV_CHECK(!parseClockTime("07:3a"));
    KV_CHECK(!parseClockTime(""));
    KV_CHECK(formatClockTime(450) == "07:30");
    KV_CHECK(formatClockTime(0) == "00:00");
    KV_CHECK(formatClockTime(1439) == "23:59");
}

KV_TEST(schedule_same_day_rows_and_day_masks) {
    constexpr uint8_t kMon = 1 << 1, kWed = 1 << 3;
    const std::vector<SharingScheduleRow> rows = {{static_cast<uint8_t>(kMon | kWed), 9 * 60, 17 * 60}};
    KV_CHECK(isSharingScheduledOff(rows, {1, 9, 0}));    // Monday 09:00, the start is inclusive
    KV_CHECK(isSharingScheduledOff(rows, {3, 16, 59}));  // Wednesday 16:59
    KV_CHECK(!isSharingScheduledOff(rows, {1, 17, 0}));  // the end is exclusive
    KV_CHECK(!isSharingScheduledOff(rows, {1, 8, 59}));
    KV_CHECK(!isSharingScheduledOff(rows, {2, 12, 0}));  // Tuesday isn't in the mask
    KV_CHECK(!isSharingScheduledOff(rows, {0, 12, 0}));  // nor Sunday
    KV_CHECK(!isSharingScheduledOff({}, {1, 12, 0}));    // no rows: never off
    KV_CHECK(!isSharingScheduledOff({{0, 0, 600}}, {1, 5, 0})); // an empty day mask does nothing
    KV_CHECK(!isSharingScheduledOff(rows, {7, 12, 0}));  // a bad weekday is not off
}

KV_TEST(schedule_rows_crossing_midnight) {
    constexpr uint8_t kFri = 1 << 5, kSat = 1 << 6;
    const std::vector<SharingScheduleRow> fri = {{kFri, 22 * 60, 7 * 60}};
    KV_CHECK(isSharingScheduledOff(fri, {5, 22, 0}));   // Friday evening
    KV_CHECK(isSharingScheduledOff(fri, {5, 23, 59}));
    KV_CHECK(isSharingScheduledOff(fri, {6, 0, 0}));    // Saturday morning belongs to Friday's row
    KV_CHECK(isSharingScheduledOff(fri, {6, 6, 59}));
    KV_CHECK(!isSharingScheduledOff(fri, {6, 7, 0}));
    KV_CHECK(!isSharingScheduledOff(fri, {5, 21, 59}));
    KV_CHECK(!isSharingScheduledOff(fri, {5, 6, 0}));   // Friday morning would be Thursday's row
    KV_CHECK(!isSharingScheduledOff(fri, {6, 22, 30})); // Saturday evening isn't in the mask
    // Saturday into Sunday wraps the week.
    const std::vector<SharingScheduleRow> sat = {{kSat, 23 * 60, 2 * 60}};
    KV_CHECK(isSharingScheduledOff(sat, {0, 1, 30}));
    KV_CHECK(!isSharingScheduledOff(sat, {1, 1, 30}));
    // from == to is a whole 24 hours from that time.
    const std::vector<SharingScheduleRow> day = {{1 << 2, 0, 0}};
    KV_CHECK(isSharingScheduledOff(day, {2, 0, 0}));
    KV_CHECK(isSharingScheduledOff(day, {2, 23, 59}));
    KV_CHECK(!isSharingScheduledOff(day, {3, 0, 0}));
    KV_CHECK(!isSharingScheduledOff(day, {1, 23, 59}));
    // Several rows: any one of them is enough.
    std::vector<SharingScheduleRow> both = fri;
    both.push_back({1 << 1, 12 * 60, 13 * 60});
    KV_CHECK(isSharingScheduledOff(both, {1, 12, 30}));
    KV_CHECK(isSharingScheduledOff(both, {6, 3, 0}));
    KV_CHECK(!isSharingScheduledOff(both, {1, 14, 0}));
}

KV_TEST(pet_settings_defaults_and_round_trip) {
    TempState t(L"settings");
    bool existed = true;
    PetSettings d = loadPetSettings(t.dir, &existed);
    KV_CHECK(!existed);
    KV_CHECK(!d.sharingEnabled && !d.privacyMode && d.sharingSchedule.empty());

    PetSettings s;
    s.sharingEnabled = true;
    s.privacyMode = true;
    s.sharingSchedule = {{0x7F, 22 * 60, 7 * 60}, {(1 << 1) | (1 << 5), 9 * 60 + 15, 17 * 60 + 45}};
    KV_CHECK(savePetSettingsAtomically(t.dir, s));
    PetSettings back = loadPetSettings(t.dir, &existed);
    KV_CHECK(existed);
    KV_CHECK(back.sharingEnabled && back.privacyMode);
    KV_CHECK(back.sharingSchedule == s.sharingSchedule);
    std::string text;
    KV_CHECK(readWholeFile(t.dir + L"\\pet-camera.json", text));
    KV_CHECK(text.find("\"from\":\"22:00\"") != std::string::npos);
    KV_CHECK(text.find("\"to\":\"07:00\"") != std::string::npos);
}

KV_TEST(pet_settings_lenient_reading) {
    PetSettings s;
    // Unknown keys (a later stage's) are ignored; bad values give defaults; bad rows are dropped.
    KV_CHECK(fromJson("{\"sharingEnabled\":\"yes\",\"privacyMode\":true,\"rollEnabled\":true,"
                      "\"outsidePort\":51000,\"sharingSchedule\":["
                      "{\"days\":2,\"from\":\"08:00\",\"to\":\"09:00\",\"note\":\"x\"},"
                      "{\"days\":2,\"from\":\"8:00\",\"to\":\"09:00\"},"
                      "{\"days\":128,\"from\":\"08:00\",\"to\":\"09:00\"},"
                      "{\"days\":-1,\"from\":\"08:00\",\"to\":\"09:00\"},"
                      "{\"days\":2,\"from\":\"08:00\"},"
                      "3,null,[]]}",
                      s));
    KV_CHECK(!s.sharingEnabled && s.privacyMode);
    KV_REQUIRE(s.sharingSchedule.size() == 1);
    KV_CHECK(s.sharingSchedule[0] == (SharingScheduleRow{2, 8 * 60, 9 * 60}));
    KV_CHECK(fromJson("{\"sharingSchedule\":{\"days\":2}}", s) && s.sharingSchedule.empty());
    // Not an object: false, and the defaults stay.
    s.sharingEnabled = true;
    KV_CHECK(!fromJson("garbage", s));
    KV_CHECK(!s.sharingEnabled);
    KV_CHECK(!fromJson("[true]", s));
    // Too many rows are cut.
    std::string many = "{\"sharingSchedule\":[";
    for (int i = 0; i < 40; ++i) many += std::string(i ? "," : "") + "{\"days\":1,\"from\":\"01:00\",\"to\":\"02:00\"}";
    many += "]}";
    KV_CHECK(fromJson(many, s) && s.sharingSchedule.size() == kMaxSharingScheduleRows);

}

// W8 (pet-camera.md §4, v3 §8.1): the outside keys go after the roll keys and fail closed. Outside listening is on
// only for a real true; a port outside 1024-65535 reads as none (a new one is picked); an address that isn't a
// public IPv4 reads as none.
KV_TEST(pet_settings_outside_keys_after_the_roll_keys_and_fail_closed) {
    TempState t(L"settings-outside");
    PetSettings d;
    KV_CHECK(!d.allowOutside && d.outsidePort == 0 && d.outsidePublicAddress.empty());

    PetSettings s;
    s.sharingEnabled = true;
    s.allowOutside = true;
    s.outsidePort = 51000;
    s.outsidePublicAddress = "203.0.113.142";
    KV_CHECK(savePetSettingsAtomically(t.dir, s));
    // The sample is a documentation address: kept only under a test program's own policy.
    const outside::AddressPolicy samples{true};
    PetSettings back = loadPetSettings(t.dir, nullptr, nullptr, nullptr, samples);
    KV_CHECK(back.allowOutside && back.outsidePort == 51000 && back.outsidePublicAddress == "203.0.113.142");
    // The product's policy reads the same file and drops it: a documentation address is never public.
    const PetSettings product = loadPetSettings(t.dir);
    KV_CHECK(product.allowOutside && product.outsidePort == 51000 && product.outsidePublicAddress.empty());
    std::string text;
    KV_CHECK(readWholeFile(t.dir + L"\\pet-camera.json", text));
    const size_t roll = text.find("\"recordSchedule\""), allow = text.find("\"allowOutside\":true"),
                 port = text.find("\"outsidePort\":51000"), addr = text.find("\"outsidePublicAddress\":\"203.0.113.142\"");
    KV_CHECK(roll != std::string::npos && allow != std::string::npos && port != std::string::npos && addr != std::string::npos);
    KV_CHECK(roll < allow && allow < port && port < addr);

    for (const char* bad : {"{\"allowOutside\":\"yes\"}", "{\"allowOutside\":1}", "{\"allowOutside\":null}",
                            "{\"allowOutside\":{}}"}) {
        PetSettings r;
        KV_CHECK_MSG(fromJson(bad, r) && !r.allowOutside, bad);
    }
    for (const char* bad : {"{\"outsidePort\":80}", "{\"outsidePort\":1023}", "{\"outsidePort\":65536}",
                            "{\"outsidePort\":70000}", "{\"outsidePort\":-5}", "{\"outsidePort\":\"51000\"}"}) {
        PetSettings r;
        KV_CHECK_MSG(fromJson(bad, r) && r.outsidePort == 0, bad);
    }
    for (const char* bad : {"{\"outsidePublicAddress\":\"192.168.1.5\"}", "{\"outsidePublicAddress\":\"100.64.1.1\"}",
                            "{\"outsidePublicAddress\":\"0.0.0.0\"}", "{\"outsidePublicAddress\":\"camera.example\"}",
                            "{\"outsidePublicAddress\":\"2001:db8::1\"}", "{\"outsidePublicAddress\":7}",
                            "{\"outsidePublicAddress\":\"203.0.113.142\"}"}) {
        PetSettings r;
        KV_CHECK_MSG(fromJson(bad, r) && r.outsidePublicAddress.empty(), bad);
    }
    PetSettings edge;
    KV_CHECK(fromJson("{\"outsidePort\":1024,\"outsidePublicAddress\":\"  203.0.113.142 \"}", edge, samples));
    KV_CHECK(edge.outsidePort == 1024 && edge.outsidePublicAddress == "203.0.113.142");
    // A W7 file has none of the keys: outside listening stays off.
    PetSettings w7;
    KV_CHECK(fromJson("{\"sharingEnabled\":true,\"rollEnabled\":true}", w7) && !w7.allowOutside && w7.outsidePort == 0);
}

// outsideMappingRemoved, a bool written after the other outside keys,
// false by default. Anything but a real false reads as true: never map on a bad value.
KV_TEST(pet_settings_outside_mapping_removed_after_the_outside_keys_and_fails_closed) {
    TempState t(L"settings-removed");
    KV_CHECK(!PetSettings{}.outsideMappingRemoved);
    PetSettings s;
    s.sharingEnabled = s.allowOutside = true;
    s.outsidePort = 51000;
    s.outsidePublicAddress = "203.0.113.142";
    s.outsideMappedPort = 51000;
    s.outsideMappingRemoved = true;
    KV_CHECK(savePetSettingsAtomically(t.dir, s));
    KV_CHECK(loadPetSettings(t.dir).outsideMappingRemoved);
    std::string text;
    KV_CHECK(readWholeFile(t.dir + L"\\pet-camera.json", text));
    const size_t addr = text.find("\"outsidePublicAddress\""), mapped = text.find("\"outsideMappedPort\":51000"),
                 removed = text.find("\"outsideMappingRemoved\":true");
    KV_CHECK(addr != std::string::npos && mapped != std::string::npos && removed != std::string::npos);
    KV_CHECK(addr < mapped && mapped < removed);
    s.outsideMappingRemoved = false;
    KV_CHECK(savePetSettingsAtomically(t.dir, s));
    KV_CHECK(!loadPetSettings(t.dir).outsideMappingRemoved);
    KV_CHECK(readWholeFile(t.dir + L"\\pet-camera.json", text) && text.find("\"outsideMappingRemoved\":false") != std::string::npos);
    for (const char* bad : {"{\"outsideMappingRemoved\":\"no\"}", "{\"outsideMappingRemoved\":0}", "{\"outsideMappingRemoved\":null}",
                            "{\"outsideMappingRemoved\":{}}", "{\"outsideMappingRemoved\":[]}"}) {
        PetSettings r;
        KV_CHECK_MSG(fromJson(bad, r) && r.outsideMappingRemoved, bad);
    }
    PetSettings off;
    KV_CHECK(fromJson("{\"outsideMappingRemoved\":false}", off) && !off.outsideMappingRemoved);
    PetSettings older; // a file from before the key: not removed
    KV_CHECK(fromJson("{\"allowOutside\":true,\"outsidePort\":51000}", older) && !older.outsideMappingRemoved);
}

// Privacy fails closed (pet-camera.md §4): a pet-camera.json that exists
// but can't be read or parsed starts with privacy mode ON; one that was read but didn't parse is kept as
// pet-camera.json.bad first. Only a missing file means the defaults (privacy off).
KV_TEST(pet_settings_an_existing_bad_or_unreadable_file_turns_privacy_on) {
    TempState t(L"settings-garbage");
    const std::string garbage = "\xff\xfe nonsense";
    KV_REQUIRE(writeText(t.dir + L"\\pet-camera.json", garbage));
    bool existed = false;
    const PetSettings g = loadPetSettings(t.dir, &existed);
    KV_CHECK(existed && !g.sharingEnabled && g.sharingSchedule.empty());
    KV_CHECK_MSG(g.privacyMode, "an unparsable pet-camera.json turned privacy off");
    std::string bad;
    KV_CHECK_MSG(readWholeFile(t.dir + L"\\pet-camera.json.bad", bad) && bad == garbage, "no pet-camera.json.bad copy");

    // Held open by another process without read sharing: privacy on, and no .bad (nothing was read).
    TempState u(L"settings-locked");
    PetSettings off;
    off.sharingEnabled = true;
    KV_REQUIRE(savePetSettingsAtomically(u.dir, off));
    {
        HANDLE h = CreateFileW((u.dir + L"\\pet-camera.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        const PetSettings l = loadPetSettings(u.dir, &existed);
        CloseHandle(h);
        KV_CHECK_MSG(l.privacyMode, "an unreadable pet-camera.json turned privacy off");
        KV_CHECK(!fileExists(u.dir + L"\\pet-camera.json.bad"));
    }
    // Readable again: what it says.
    const PetSettings back = loadPetSettings(u.dir, &existed);
    KV_CHECK(existed && back.sharingEnabled && !back.privacyMode);

    // Missing: the defaults, privacy off.
    TempState v(L"settings-missing");
    const PetSettings d = loadPetSettings(v.dir, &existed);
    KV_CHECK(!existed && !d.privacyMode && !d.sharingEnabled);
}

// devices.json that exists but can't be read (another process holds it without read sharing) is not
// corruption: nothing is rebuilt or saved over it, nobody is admitted, and a later read gets the real list.
KV_TEST(a_devices_json_that_cant_be_read_is_left_alone_and_admits_nobody) {
    TempState t(L"devices-locked");
    DeviceStore store(t.dir);
    HostPersistedState p;
    setUpTwo(t.dir, store, p);
    std::string before;
    KV_REQUIRE(readWholeFile(t.dir + L"\\devices.json", before));
    // pairing.json pins owner A (the mirror): a migration would make it the only device, losing B.
    {
        HANDLE h = CreateFileW((t.dir + L"\\devices.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        DeviceStore locked(t.dir);
        HostPersistedState lp = onDisk(t.dir);
        locked.loadAndReconcile(lp, 9);
        CloseHandle(h);
        KV_CHECK_MSG(locked.empty(), "a device was admitted from a devices.json that couldn't be read");
        KV_CHECK(!locked.findByHash(hashOf(1)) && !locked.findByHash(hashOf(2)));
    }
    std::string after;
    KV_REQUIRE(readWholeFile(t.dir + L"\\devices.json", after));
    KV_CHECK_MSG(after == before, "devices.json was overwritten");
    KV_CHECK(!fileExists(t.dir + L"\\devices.json.bad"));
    // Once it can be read: the real list, both devices.
    DeviceStore again(t.dir);
    HostPersistedState ap = onDisk(t.dir);
    again.loadAndReconcile(ap, 10);
    KV_CHECK(again.size() == 2 && again.findByHash(hashOf(1)) && again.findByHash(hashOf(2)));
}

// A devices.json that was read but doesn't parse goes to devices.json.bad, and is rebuilt only once that
// copy exists: when the copy fails, it is left alone like an unreadable one.
KV_TEST(an_unparsable_devices_json_is_rebuilt_only_after_its_bad_copy_is_made) {
    TempState t(L"devices-nocopy");
    HostPersistedState p = pairingState("S1", hashOf(4));
    KV_REQUIRE(saveHostStateAtomically(t.dir, p));
    const std::string garbage = "\x01\x02 not json {{{";
    KV_REQUIRE(writeText(t.dir + L"\\devices.json", garbage));
    KV_REQUIRE(writeText(t.dir + L"\\devices.json.bad", "an older copy"));
    HANDLE h = CreateFileW((t.dir + L"\\devices.json.bad").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        DeviceStore store(t.dir);
        HostPersistedState lp = onDisk(t.dir);
        store.loadAndReconcile(lp, 1);
        KV_CHECK_MSG(store.empty(), "rebuilt from pairing.json without a .bad copy");
    }
    CloseHandle(h);
    std::string now;
    KV_REQUIRE(readWholeFile(t.dir + L"\\devices.json", now));
    KV_CHECK_MSG(now == garbage, "devices.json was overwritten without a .bad copy");
    // The copy works now: kept aside, then rebuilt (row 1).
    DeviceStore store(t.dir);
    HostPersistedState lp = onDisk(t.dir);
    KV_CHECK(store.loadAndReconcile(lp, 2).which == DeviceStore::ReconcileCase::Migrated);
    std::string bad;
    KV_CHECK(readWholeFile(t.dir + L"\\devices.json.bad", bad) && bad == garbage);
    KV_CHECK(store.size() == 1 && store.findByHash(hashOf(4)));
}

// ---- .bad copies are numbered: a later one never overwrites an earlier one ------------------------------------

// Seven different bad files, one after another: the newest is <name>.bad, the four before it are .bad.1 (newer) to
// .bad.4 (older), the two oldest are dropped, and no .bad.5 or .bad.tmp is left. The same bytes again copy
// nothing. On the old code each copy went over pairing.json.bad, so a reset's copy of the old identity was lost to
// the next bad start.
KV_TEST(bad_copies_are_numbered_and_the_oldest_beyond_five_is_dropped) {
    TempState t(L"bad-copies");
    const std::wstring f = t.dir + L"\\pairing.json";
    auto body = [](int i) { return "bad file " + std::to_string(i); };
    for (int i = 0; i < 7; ++i) {
        KV_REQUIRE(writeText(f, body(i)));
        std::string what;
        KV_REQUIRE(keepBadCopy(t.dir, L"pairing.json", &what));
        KV_CHECK_MSG(what == "kept as pairing.json.bad", what);
    }
    auto bytes = [&](const std::wstring& name) {
        std::string b;
        return readWholeFile(t.dir + L"\\" + name, b) ? b : std::string("(none)");
    };
    KV_CHECK(bytes(L"pairing.json.bad") == body(6));
    for (int i = 1; i < kBadCopiesKept; ++i) {
        KV_CHECK_MSG(bytes(L"pairing.json.bad." + std::to_wstring(i)) == body(6 - i), bytes(L"pairing.json.bad." + std::to_wstring(i)));
    }
    KV_CHECK(!fileExists(t.dir + L"\\pairing.json.bad.5") && !fileExists(t.dir + L"\\pairing.json.bad.tmp"));
    KV_CHECK(bytes(L"pairing.json") == body(6)); // the file itself is only read

    // Bytes a kept copy already holds: nothing moves.
    KV_REQUIRE(writeText(f, body(4)));
    std::string what;
    KV_CHECK(keepBadCopy(t.dir, L"pairing.json", &what));
    KV_CHECK_MSG(what == "pairing.json.bad.2 already held it", what);
    KV_CHECK(bytes(L"pairing.json.bad") == body(6) && bytes(L"pairing.json.bad.4") == body(2));

    // A copy that can't be made (.bad.tmp is a folder): false with the Win32 error, and no kept copy moves.
    KV_REQUIRE(writeText(f, "another"));
    KV_REQUIRE(CreateDirectoryW((t.dir + L"\\pairing.json.bad.tmp").c_str(), nullptr));
    KV_CHECK(!keepBadCopy(t.dir, L"pairing.json", &what));
    KV_CHECK_MSG(what.find("keeping a copy as pairing.json.bad failed (Win32 error") == 0, what);
    KV_CHECK(bytes(L"pairing.json.bad") == body(6) && bytes(L"pairing.json.bad.4") == body(2));
    RemoveDirectoryW((t.dir + L"\\pairing.json.bad.tmp").c_str());
}

// ---- fileState: only "not found" means missing ---------------------------------------------------------------

namespace {
// GetFileAttributesW can fail for reasons other than "not found" (access denied, a network or filter-driver
// error); a test can't make that happen reliably, so the seam answers for one file name with that error.
std::wstring g_failing_name;
DWORD g_failing_error = ERROR_ACCESS_DENIED;

unsigned long failingAttributes(const std::wstring& path, unsigned long* err) {
    const size_t n = g_failing_name.size();
    if (path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, g_failing_name.c_str()) == 0) {
        *err = g_failing_error;
        return INVALID_FILE_ATTRIBUTES;
    }
    const DWORD a = GetFileAttributesW(path.c_str());
    *err = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    return a;
}

struct AttributesFailFor {
    AttributesFailFor(const wchar_t* name, DWORD error) {
        g_failing_name = std::wstring(L"\\") + name;
        g_failing_error = error;
        setFileAttributesProbeForTest(&failingAttributes);
    }
    ~AttributesFailFor() { setFileAttributesProbeForTest(nullptr); }
};
} // namespace

KV_TEST(file_state_says_missing_only_for_not_found) {
    TempState t(L"filestate");
    KV_REQUIRE(writeText(t.dir + L"\\a.json", "{}"));
    unsigned long e = 1;
    KV_CHECK(fileState(t.dir + L"\\a.json", &e) == FileState::Present && e == 0);
    KV_CHECK(fileExists(t.dir + L"\\a.json"));
    KV_CHECK(fileState(t.dir + L"\\none.json", &e) == FileState::Missing && e == ERROR_FILE_NOT_FOUND);
    KV_CHECK(fileState(t.dir + L"\\no-such-folder\\none.json", &e) == FileState::Missing && e == ERROR_PATH_NOT_FOUND);
    KV_CHECK(!fileExists(t.dir + L"\\none.json"));
    // A folder where the file should be is not a missing file.
    KV_REQUIRE(CreateDirectoryW((t.dir + L"\\folder.json").c_str(), nullptr));
    KV_CHECK(fileState(t.dir + L"\\folder.json", &e) == FileState::Unknown && e != 0);
    KV_CHECK(!fileExists(t.dir + L"\\folder.json"));
    RemoveDirectoryW((t.dir + L"\\folder.json").c_str());
    // Any other error: Unknown, with the error.
    {
        AttributesFailFor fail(L"a.json", ERROR_ACCESS_DENIED);
        KV_CHECK(fileState(t.dir + L"\\a.json", &e) == FileState::Unknown && e == ERROR_ACCESS_DENIED);
        KV_CHECK(!fileExists(t.dir + L"\\a.json"));
        KV_CHECK(fileState(t.dir + L"\\none.json", &e) == FileState::Missing); // the seam passes other names on
    }
    KV_CHECK(fileState(t.dir + L"\\a.json") == FileState::Present);
}

// Before the fix fileExists took any GetFileAttributesW failure for "missing": pet-camera.json then gave the
// defaults (privacy OFF), and devices.json was rebuilt from pairing.json and saved over the real list.
KV_TEST(an_existence_check_that_fails_otherwise_fails_closed_in_each_loader) {
    // pet-camera.json says privacy off; its existence can't be checked: privacy on, unread (never written
    // over), the error carries the Win32 code, and nothing is kept as .bad (nothing was read).
    TempState s(L"filestate-settings");
    PetSettings off;
    off.sharingEnabled = true;
    KV_REQUIRE(savePetSettingsAtomically(s.dir, off));
    {
        AttributesFailFor fail(L"pet-camera.json", ERROR_ACCESS_DENIED);
        bool existed = false, unread = false;
        std::string err;
        const PetSettings got = loadPetSettings(s.dir, &existed, &err, &unread);
        KV_CHECK_MSG(got.privacyMode, "privacy is off when pet-camera.json's existence couldn't be checked");
        KV_CHECK(existed && unread);
        KV_CHECK_MSG(err.find("Win32 error 5") != std::string::npos, err);
        KV_CHECK(!fileExists(s.dir + L"\\pet-camera.json.bad"));
    }
    KV_CHECK(!loadPetSettings(s.dir).privacyMode); // checkable again: what it says

    // devices.json lists two devices; its existence can't be checked: nobody is listed, the read failed (the
    // host reads it again later), and the file is byte for byte as it was.
    TempState d(L"filestate-devices");
    DeviceStore store(d.dir);
    HostPersistedState p;
    setUpTwo(d.dir, store, p);
    std::string before;
    KV_REQUIRE(readWholeFile(d.dir + L"\\devices.json", before));
    {
        AttributesFailFor fail(L"devices.json", ERROR_NETWORK_BUSY);
        DeviceStore again(d.dir);
        HostPersistedState lp = onDisk(d.dir);
        const auto r = again.loadAndReconcile(lp, 9);
        KV_CHECK_MSG(r.readFailed && r.which == DeviceStore::ReconcileCase::Unread,
                     "devices.json whose existence couldn't be checked was taken as missing");
        KV_CHECK(again.empty());
        KV_CHECK_MSG(r.readError.find("Win32 error " + std::to_string(ERROR_NETWORK_BUSY)) != std::string::npos, r.readError);
    }
    std::string after;
    KV_REQUIRE(readWholeFile(d.dir + L"\\devices.json", after));
    KV_CHECK_MSG(after == before, "devices.json was written over");
    DeviceStore ok(d.dir);
    HostPersistedState op = onDisk(d.dir);
    ok.loadAndReconcile(op, 10);
    KV_CHECK(ok.size() == 2);
}

// ---- pet-camera.json: a privacyMode that is there but isn't a bool is ON (pet_settings.cpp) -------------------

KV_TEST(pet_settings_a_privacy_mode_that_isnt_a_bool_is_on) {
    for (const char* v : {"\"no\"", "\"false\"", "0", "null", "{}", "[]"}) {
        PetSettings s;
        KV_CHECK(fromJson(std::string("{\"sharingEnabled\":true,\"privacyMode\":") + v + "}", s));
        KV_CHECK_MSG(s.privacyMode, std::string("privacyMode ") + v + " didn't turn privacy on");
        KV_CHECK(s.sharingEnabled); // the rest of the file still counts
    }
    PetSettings absent;
    KV_CHECK(fromJson("{\"sharingEnabled\":true}", absent));
    KV_CHECK_MSG(!absent.privacyMode, "an absent privacyMode isn't the default (off)");
    PetSettings f, t;
    KV_CHECK(fromJson("{\"privacyMode\":false}", f) && !f.privacyMode);
    KV_CHECK(fromJson("{\"privacyMode\":true}", t) && t.privacyMode);
}

KV_TEST_MAIN()
