#include "device_store.hpp"

#include "crypto_identity.hpp"
#include "json_lite.hpp"
#include "state_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace kvhost {

namespace {

constexpr size_t kHashBytes = 32;

// ---- UTF-8 ------------------------------------------------------------------------------------

// Decodes one code point at s[i]. Invalid, overlong, surrogate or out-of-range sequences give
// U+FFFD and consume one byte, so every input decodes to something.
uint32_t decodeOne(const std::string& s, size_t& i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int len = 0;
    uint32_t cp = 0, min = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else { ++i; return 0xFFFD; }
    if (i + len > s.size()) { ++i; return 0xFFFD; }
    for (int k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
    i += len;
    return cp;
}

void encodeOne(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Control characters, plus the line and paragraph separators, which would break a one-line label.
bool isControl(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
}

// The bidirectional marks, embeddings, overrides and isolates: a name holding one can show its text out of order
// (an override can make a name read as another's). Dropped, not turned into spaces.
bool isBidiControl(uint32_t cp) {
    return cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069);
}

// Characters that show nothing: the soft hyphen, the Mongolian vowel separator, the zero-width space, the word joiner
// and the invisible operators, and the zero-width no-break space (U+FEFF) anywhere in a name. A name holding one
// looks like the same name without it, so it would pass the duplicate check. Dropped. The zero-width joiner and
// non-joiner (U+200D, U+200C), which emoji and some scripts need, are kept.
bool isInvisible(uint32_t cp) {
    return cp == 0x00AD || cp == 0x180E || cp == 0x200B || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF;
}

bool isSpace(uint32_t cp) {
    return cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

void trimSpaces(std::vector<uint32_t>& cps) {
    size_t a = 0, e = cps.size();
    while (a < e && isSpace(cps[a])) ++a;
    while (e > a && isSpace(cps[e - 1])) --e;
    cps = std::vector<uint32_t>(cps.begin() + static_cast<std::ptrdiff_t>(a), cps.begin() + static_cast<std::ptrdiff_t>(e));
}

size_t utf8Length(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

// ---- devices.json reading ----------------------------------------------------------------------

std::optional<std::vector<uint8_t>> hashFromValue(const json::Value& v) {
    std::vector<uint8_t> h;
    if (!v.asBytes(h) || h.size() != kHashBytes) return std::nullopt;
    return h;
}

PairingSnapshot snapshotFromValue(const json::Value* v) {
    PairingSnapshot s;
    if (!v || v->type != json::Value::Type::Object) return s;
    if (const json::Value* id = v->get("serviceId")) id->asString(s.serviceId);
    if (const json::Value* m = v->get("mirror")) s.mirror = hashFromValue(*m);
    return s;
}

std::string snapshotJson(const PairingSnapshot& s) {
    json::ObjectWriter w;
    w.field("serviceId", s.serviceId);
    if (s.mirror) w.bytes("mirror", *s.mirror);
    return w.finish();
}

std::string shortId(const std::optional<std::vector<uint8_t>>& h) {
    return h ? deviceIdFromHash(*h) : std::string("none");
}

} // namespace

// ---- free functions ---------------------------------------------------------------------------

const char* deviceKindString(DeviceKind k) {
    switch (k) {
        case DeviceKind::IPhone: return "iphone";
        case DeviceKind::IPad: return "ipad";
        case DeviceKind::Mac: return "mac";
        default: return "other";
    }
}

DeviceKind deviceKindFromString(const std::string& s) {
    if (s == "iphone") return DeviceKind::IPhone;
    if (s == "ipad") return DeviceKind::IPad;
    if (s == "mac") return DeviceKind::Mac;
    return DeviceKind::Other;
}

std::string deviceIdFromHash(const std::vector<uint8_t>& h) {
    return hexLower(h.data(), std::min<size_t>(8, h.size()));
}

std::optional<std::string> sanitizeDeviceName(const std::string& raw) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < raw.size();) {
        const uint32_t cp = decodeOne(raw, i);
        if (isBidiControl(cp) || isInvisible(cp)) continue;
        cps.push_back(isControl(cp) ? 0x20 : cp);
    }
    trimSpaces(cps);
    // Cut at a character boundary, then trim again in case the cut left a trailing space.
    size_t bytes = 0, keep = 0;
    while (keep < cps.size() && bytes + utf8Length(cps[keep]) <= kMaxDeviceNameBytes) bytes += utf8Length(cps[keep++]);
    cps.resize(keep);
    trimSpaces(cps);
    if (cps.empty()) return std::nullopt;
    std::string out;
    for (uint32_t cp : cps) encodeOne(cp, out);
    return out;
}

std::string nextDefaultDeviceName(const std::vector<std::string>& taken) { return uniqueDeviceName("iPhone", taken); }

std::string uniqueDeviceName(const std::string& name, const std::vector<std::string>& taken) {
    const auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    if (!used(name)) return name;
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < name.size();) cps.push_back(decodeOne(name, i));
    for (size_t n = 2;; ++n) {
        const std::string suffix = " " + std::to_string(n);
        // The name cut at a character boundary so that it and the suffix fit in 64 bytes.
        std::vector<uint32_t> base;
        size_t bytes = 0;
        for (uint32_t cp : cps) {
            if (bytes + utf8Length(cp) + suffix.size() > kMaxDeviceNameBytes) break;
            bytes += utf8Length(cp);
            base.push_back(cp);
        }
        trimSpaces(base);
        std::string out;
        for (uint32_t cp : base) encodeOne(cp, out);
        out += suffix;
        if (!used(out)) return out;
    }
}

// ---- DeviceStore -------------------------------------------------------------------------------

DeviceStore::DeviceStore(std::wstring stateDir) : dir_(std::move(stateDir)) {}

const DeviceRecord* DeviceStore::findByHash(const std::vector<uint8_t>& h) const {
    // Every record is compared, so the time taken doesn't say which one matched or how far it got.
    const DeviceRecord* hit = nullptr;
    for (const auto& d : devices_) {
        if (constantTimeEqual(d.certSHA256, h) && !hit) hit = &d;
    }
    return hit;
}

const DeviceRecord* DeviceStore::findByDeviceId(const std::string& id) const {
    for (const auto& d : devices_) {
        if (d.deviceId == id) return &d;
    }
    return nullptr;
}

DeviceRecord* DeviceStore::findMutable(const std::string& id) {
    for (auto& d : devices_) {
        if (d.deviceId == id) return &d;
    }
    return nullptr;
}

bool DeviceStore::isRevoked(const std::vector<uint8_t>& h) const {
    return std::find(revoked_.begin(), revoked_.end(), h) != revoked_.end();
}

std::optional<std::vector<uint8_t>> DeviceStore::mirror() const {
    const DeviceRecord* best = nullptr;
    for (const auto& d : devices_) {
        if (d.role != PetRole::Owner) continue;
        if (!best || d.pairedAtUnixMs >= best->pairedAtUnixMs) best = &d;
    }
    if (!best) return std::nullopt;
    return best->certSHA256;
}

std::string DeviceStore::nextDefaultName() const {
    std::vector<std::string> taken;
    for (const auto& d : devices_) taken.push_back(d.name);
    return nextDefaultDeviceName(taken);
}

void DeviceStore::addToRevoked(const std::vector<uint8_t>& h) {
    removeFromRevoked(h); // a second revoke moves it to the newest end
    revoked_.push_back(h);
    while (revoked_.size() > kMaxRevokedHashes) revoked_.erase(revoked_.begin());
}

void DeviceStore::removeFromRevoked(const std::vector<uint8_t>& h) {
    revoked_.erase(std::remove(revoked_.begin(), revoked_.end(), h), revoked_.end());
}

void DeviceStore::addMigratedOwner(const std::vector<uint8_t>& h, uint64_t nowUnixMs) {
    DeviceRecord r;
    r.certSHA256 = h;
    r.deviceId = deviceIdFromHash(h);
    r.name = nextDefaultName();
    r.role = PetRole::Owner;
    r.kind = DeviceKind::IPhone;
    r.pairedAtUnixMs = nowUnixMs;
    devices_.push_back(std::move(r));
}

DeviceStore::AddResult DeviceStore::addPairedDevice(const NewDevice& d, const DeviceRecord** out) {
    if (out) *out = nullptr;
    if (d.certSHA256.size() != kHashBytes || d.role == PetRole::Unknown) return AddResult::Invalid;
    for (auto& existing : devices_) {
        if (existing.certSHA256 == d.certSHA256) {
            if (d.protocolVersion == 3) existing.lastProtocolVersion = 3;
            removeFromRevoked(d.certSHA256);
            if (out) *out = &existing;
            return AddResult::AlreadyPaired;
        }
    }
    if (full()) return AddResult::TooManyDevices;
    DeviceRecord r;
    r.certSHA256 = d.certSHA256;
    r.deviceId = deviceIdFromHash(d.certSHA256);
    std::optional<std::string> name = d.deviceName ? sanitizeDeviceName(*d.deviceName) : std::nullopt;
    if (name) {
        // A device can't take a name already on the list (another device's, the owner's phone's): it gets the
        // next free "<name> 2", "<name> 3" ... A rename on the PC is the PC user's choice and isn't changed.
        std::vector<std::string> taken;
        for (const auto& existing : devices_) taken.push_back(existing.name);
        r.name = uniqueDeviceName(*name, taken);
    } else {
        r.name = nextDefaultName();
    }
    r.role = d.role;
    // Old phones send no kind; they are all the iPhone app.
    r.kind = d.deviceKind ? deviceKindFromString(*d.deviceKind) : DeviceKind::IPhone;
    r.pairedAtUnixMs = d.nowUnixMs;
    if (d.protocolVersion == 3) r.lastProtocolVersion = 3;
    removeFromRevoked(d.certSHA256);
    devices_.push_back(std::move(r));
    if (out) *out = &devices_.back();
    return AddResult::Added;
}

DeviceStore::RePairResult DeviceStore::rePair(const std::string& deviceId, const std::vector<uint8_t>& h,
                                              uint64_t nowUnixMs, std::optional<int> protocolVersion,
                                              std::optional<std::string> deviceKind) {
    if (h.size() != kHashBytes) return RePairResult::Invalid;
    DeviceRecord* r = findMutable(deviceId);
    if (!r) return RePairResult::NotFound;
    for (const auto& other : devices_) {
        if (&other != r && other.certSHA256 == h) return RePairResult::HashInUse;
    }
    r->certSHA256 = h;
    r->deviceId = deviceIdFromHash(h);
    r->pairedAtUnixMs = nowUnixMs;
    if (deviceKind) r->kind = deviceKindFromString(*deviceKind);
    r->lastProtocolVersion = protocolVersion == 3 ? std::optional<int>(3) : std::nullopt;
    removeFromRevoked(h);
    return RePairResult::Replaced;
}

DeviceStore::RevokeResult DeviceStore::revoke(const std::string& deviceId) {
    RevokeResult res;
    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
        if (it->deviceId != deviceId) continue;
        res.found = true;
        res.removed = *it;
        addToRevoked(it->certSHA256);
        devices_.erase(it);
        break;
    }
    res.nowEmpty = devices_.empty();
    return res;
}

DeviceStore::RevokeResult DeviceStore::revokeByHash(const std::vector<uint8_t>& h) {
    const DeviceRecord* r = findByHash(h);
    if (!r) {
        RevokeResult res;
        res.nowEmpty = devices_.empty();
        return res;
    }
    return revoke(std::string(r->deviceId));
}

size_t DeviceStore::revokeAll() {
    const size_t n = devices_.size();
    for (const auto& d : devices_) addToRevoked(d.certSHA256);
    devices_.clear();
    return n;
}

bool DeviceStore::rename(const std::string& deviceId, const std::string& newName) {
    DeviceRecord* r = findMutable(deviceId);
    const std::optional<std::string> name = sanitizeDeviceName(newName);
    if (!r || !name) return false;
    r->name = *name;
    return true;
}

bool DeviceStore::setRole(const std::string& deviceId, PetRole role) {
    DeviceRecord* r = findMutable(deviceId);
    if (!r || role == PetRole::Unknown) return false;
    r->role = role;
    return true;
}

bool DeviceStore::touch(const std::vector<uint8_t>& h, uint64_t nowUnixMs, std::optional<int> protocolVersion) {
    for (auto& d : devices_) {
        if (d.certSHA256 != h) continue;
        d.lastSeenUnixMs = nowUnixMs;
        if (protocolVersion) d.lastProtocolVersion = *protocolVersion;
        touched_ = true;
        ++touchSeq_;
        return true;
    }
    return false;
}

// ---- files --------------------------------------------------------------------------------------

std::string DeviceStore::toJson(const PairingSnapshot& written, const PairingSnapshot& before) const {
    std::string list = "[";
    for (size_t i = 0; i < devices_.size(); ++i) {
        const DeviceRecord& d = devices_[i];
        json::ObjectWriter w;
        w.bytes("certSHA256", d.certSHA256)
            .field("deviceId", d.deviceId)
            .field("name", d.name)
            .field("role", petRoleString(d.role))
            .field("kind", deviceKindString(d.kind))
            .field("pairedAtUnixMs", d.pairedAtUnixMs);
        if (d.lastSeenUnixMs) w.field("lastSeenUnixMs", *d.lastSeenUnixMs);
        if (d.lastProtocolVersion) w.field("lastProtocolVersion", *d.lastProtocolVersion);
        if (i) list += ',';
        list += w.finish();
    }
    list += ']';
    return json::ObjectWriter()
        .field("version", 1)
        .raw("written", snapshotJson(written))
        .raw("before", snapshotJson(before))
        .raw("devices", list)
        .bytesArray("revoked", revoked_)
        .finish();
}

bool DeviceStore::writeDevicesFile(const PairingSnapshot& written, std::string* err) {
    return writeOwnerOnlyFileAtomically(dir_, L"devices.json", toJson(written, onDisk_), err);
}

bool DeviceStore::save(HostPersistedState& pairing, std::string* err) {
    const PairingSnapshot written{pairing.serviceId, mirror()};
    if (!writeDevicesFile(written, err)) return false;
    touched_ = false;
    pairing.pinnedClientCertSHA256 = written.mirror;
    if (crashBetweenWrites_) {
        if (err) *err = "simulated crash between devices.json and pairing.json";
        return false;
    }
    if (!saveHostStateAtomically(dir_, pairing, err)) return false;
    onDisk_ = written;
    return true;
}

bool DeviceStore::saveDeviceListOnly(std::string* err) {
    if (mirror() != onDisk_.mirror) {
        if (err) *err = "the mirror changed: use save()";
        return false;
    }
    // pairing.json doesn't change, so it already holds `written`; before is the same.
    if (!writeDevicesFile(onDisk_, err)) return false;
    touched_ = false;
    return true;
}

void DeviceStore::adoptSave(const DeviceStore& saved) {
    onDisk_ = saved.onDisk_;
    if (!saved.touched_ && touchSeq_ == saved.touchSeq_) touched_ = false;
}

DeviceStore::ReconcileResult DeviceStore::loadAndReconcile(HostPersistedState& pairing, uint64_t nowUnixMs) {
    ReconcileResult res;
    devices_.clear();
    revoked_.clear();
    touched_ = false;

    std::optional<std::vector<uint8_t>> pin = pairing.pinnedClientCertSHA256;
    if (pin && pin->size() != kHashBytes) {
        res.log.push_back("pairing.json: the pin is not a 32-byte hash; it is ignored");
        pin.reset();
    }
    const PairingSnapshot current{pairing.serviceId, pin};
    onDisk_ = current;

    // ---- read devices.json leniently ----
    std::string text;
    const std::wstring path = devicesPath();
    unsigned long state_error = 0;
    const FileState state = fileState(path, &state_error);
    json::Value root;
    bool readable = false;
    // An I/O failure is not corruption: the list on disk may be fine, so nothing is rebuilt or saved over
    // it and nobody is admitted (fail closed). The caller reads it again later.
    auto unread = [&](const std::string& why) {
        res.which = ReconcileCase::Unread;
        res.readFailed = true;
        res.readError = why;
        res.log.push_back(why + "; nobody is admitted and nothing is written until it can be read");
        return res;
    };
    if (state == FileState::Unknown) {
        // Whether it is there couldn't be checked (an error other than "not found"): taken as there but
        // unreadable, never as missing (which would rebuild the list from pairing.json and save over it).
        return unread("devices.json couldn't be checked (Win32 error " + std::to_string(state_error) + ")");
    }
    if (state == FileState::Present) {
        if (!readWholeFile(path, text)) {
            const DWORD e = GetLastError();
            return unread("devices.json couldn't be read (Win32 error " + std::to_string(e) + ")");
        }
        readable = json::parse(text, root) && root.type == json::Value::Type::Object;
        if (!readable) {
            // Read but not parsed: kept for a human to look at, and only once that copy exists is the list
            // rebuilt from pairing.json. Without the copy it is left alone like an unreadable file.
            std::string kept;
            if (!keepBadCopy(dir_, L"devices.json", &kept)) {
                return unread("devices.json couldn't be parsed, and " + kept);
            }
            res.log.push_back("devices.json could not be parsed (" + kept + ") and was rebuilt from pairing.json");
        }
    }

    PairingSnapshot written, before;
    if (readable) {
        written = snapshotFromValue(root.get("written"));
        before = snapshotFromValue(root.get("before"));
        if (const json::Value* rv = root.get("revoked"); rv && rv->type == json::Value::Type::Array) {
            for (const auto& item : rv->items) {
                if (auto h = hashFromValue(item); h && !isRevoked(*h)) revoked_.push_back(std::move(*h));
            }
            while (revoked_.size() > kMaxRevokedHashes) revoked_.erase(revoked_.begin());
        }
        std::vector<size_t> unnamed;
        if (const json::Value* dv = root.get("devices"); dv && dv->type == json::Value::Type::Array) {
            for (const auto& item : dv->items) {
                if (item.type != json::Value::Type::Object) continue;
                const json::Value* hv = item.get("certSHA256");
                std::optional<std::vector<uint8_t>> h = hv ? hashFromValue(*hv) : std::nullopt;
                if (!h) continue;
                bool dup = false;
                for (const auto& d : devices_) dup = dup || d.certSHA256 == *h;
                if (dup) continue; // the first record for a hash wins
                if (isRevoked(*h)) {
                    res.log.push_back("devices.json: device " + deviceIdFromHash(*h) +
                                      " is also on the revoked list; it stays revoked");
                    continue;
                }
                if (devices_.size() >= kMaxPairedDevices) continue;
                DeviceRecord r;
                r.certSHA256 = *h;
                r.deviceId = deviceIdFromHash(*h);
                std::string s;
                if (const json::Value* v = item.get("name"); v && v->asString(s)) {
                    if (auto n = sanitizeDeviceName(s)) r.name = *n;
                }
                // An unreadable role is a viewer: the least it can mean.
                r.role = PetRole::Viewer;
                if (const json::Value* v = item.get("role"); v && v->asString(s) && petRoleFromString(s) == PetRole::Owner) {
                    r.role = PetRole::Owner;
                }
                if (const json::Value* v = item.get("kind"); v && v->asString(s)) r.kind = deviceKindFromString(s);
                uint64_t u = 0;
                if (const json::Value* v = item.get("pairedAtUnixMs"); v && v->asUInt64(u)) r.pairedAtUnixMs = u;
                if (const json::Value* v = item.get("lastSeenUnixMs"); v && v->asUInt64(u)) r.lastSeenUnixMs = u;
                int64_t i = 0;
                if (const json::Value* v = item.get("lastProtocolVersion");
                    v && v->asInt64(i) && i >= 1 && i <= 1000) {
                    r.lastProtocolVersion = static_cast<int>(i);
                }
                if (r.name.empty()) unnamed.push_back(devices_.size());
                devices_.push_back(std::move(r));
            }
        }
        for (size_t idx : unnamed) {
            devices_[idx].name = nextDefaultName();
            res.log.push_back("devices.json: device " + devices_[idx].deviceId + " had no usable name; now \"" +
                              devices_[idx].name + "\"");
        }
    }

    // ---- the reconciliation table ----
    bool rewrite = false;
    if (!readable) {
        res.which = ReconcileCase::Migrated;
        if (pin) {
            addMigratedOwner(*pin, nowUnixMs);
            res.log.push_back("First start with the device list: the paired phone became owner device \"" +
                              devices_.back().name + "\" (" + devices_.back().deviceId + ")");
        } else {
            res.log.push_back("First start with the device list: no phone was paired; the list starts empty");
        }
        rewrite = true;
    } else if (current == written) {
        res.which = ReconcileCase::Consistent;
    } else if (current == before) {
        res.which = ReconcileCase::RepairedPairing;
        res.log.push_back("pairing.json was not updated after the last devices.json save; rewriting it (serviceId " +
                          written.serviceId + ", mirror " + shortId(written.mirror) + ")");
        pairing.serviceId = written.serviceId;
        pairing.pinnedClientCertSHA256 = written.mirror;
        std::string err;
        if (saveHostStateAtomically(dir_, pairing, &err)) {
            res.pairingRewritten = true;
            onDisk_ = written;
        } else {
            res.saveError = err;
            res.log.push_back("Rewriting pairing.json failed: " + err);
        }
    } else if (current.serviceId != written.serviceId) {
        res.which = ReconcileCase::DowngradeUnpair;
        res.log.push_back("pairing.json has a new serviceId (an older build unpaired): " +
                          std::to_string(devices_.size()) + " device(s) dropped");
        devices_.clear();
        if (pin && isRevoked(*pin)) {
            res.log.push_back("The pin " + deviceIdFromHash(*pin) + " is a revoked device; it is not admitted");
        } else if (pin) {
            addMigratedOwner(*pin, nowUnixMs);
            res.log.push_back("The pin from pairing.json became owner device \"" + devices_.back().name + "\" (" +
                              devices_.back().deviceId + ")");
        }
        rewrite = true;
    } else if (pin) {
        res.which = ReconcileCase::DowngradePair;
        if (isRevoked(*pin)) {
            res.log.push_back("pairing.json pins a revoked device (" + deviceIdFromHash(*pin) + "); it is ignored");
        } else if (findByHash(*pin)) {
            res.log.push_back("pairing.json pins listed device " + deviceIdFromHash(*pin) + "; nothing to change");
        } else {
            if (written.mirror) {
                for (auto it = devices_.begin(); it != devices_.end(); ++it) {
                    if (it->certSHA256 != *written.mirror) continue;
                    res.log.push_back("An older build replaced the pin: device \"" + it->name + "\" (" + it->deviceId +
                                      ") dropped");
                    devices_.erase(it);
                    break;
                }
            }
            if (full()) {
                res.log.push_back("An older build paired " + deviceIdFromHash(*pin) +
                                  " but the list is full; it is not added");
            } else {
                addMigratedOwner(*pin, nowUnixMs);
                res.log.push_back("An older build paired a phone: it became owner device \"" + devices_.back().name +
                                  "\" (" + devices_.back().deviceId + ")");
            }
        }
        rewrite = true;
    } else {
        res.which = ReconcileCase::DowngradeClear;
        res.log.push_back("pairing.json's pin was cleared (an older build unpaired): " +
                          std::to_string(devices_.size()) + " device(s) dropped");
        devices_.clear();
        rewrite = true;
    }

    // A list that loaded differently from what was saved (a dropped bad entry) can change the mirror.
    if (!rewrite && res.saveError.empty() && mirror() != onDisk_.mirror) {
        res.log.push_back("The mirror in pairing.json doesn't match the device list; rewriting both files");
        rewrite = true;
    }
    if (rewrite) {
        std::string err;
        if (save(pairing, &err)) {
            res.devicesRewritten = true;
            res.pairingRewritten = true;
        } else {
            res.saveError = err;
            res.log.push_back("Saving the reconciled device list failed: " + err);
        }
    } else if (res.which == ReconcileCase::Consistent) {
        pairing.pinnedClientCertSHA256 = pin;
    }
    return res;
}

} // namespace kvhost
