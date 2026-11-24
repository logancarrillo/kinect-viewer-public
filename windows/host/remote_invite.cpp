#include "remote_invite.hpp"

#include "crypto_identity.hpp"
#include "json_lite.hpp"
#include "outside_address.hpp"
#include "pairing_payload.hpp"
#include "state_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h> // CryptProtectData (crypt32)

#include <cstdio>

namespace kvhost::outside {

namespace {

// The invite token is kept in invite.json wrapped with DPAPI for this Windows user (CryptProtectData, with this
// app's entropy), so a copy of the state folder (a backup, a disk image without this user's sign-in) holds no
// token that can be redeemed. One that can't be unwrapped (another user or PC, a damaged file) reads as a closed
// invite: the PC makes a new one, and no paired device is touched.
const char kInviteTokenEntropy[] = "kinect-viewer invite token v1";

DATA_BLOB entropyBlob() {
    return DATA_BLOB{static_cast<DWORD>(sizeof(kInviteTokenEntropy) - 1),
                     reinterpret_cast<BYTE*>(const_cast<char*>(kInviteTokenEntropy))};
}

bool wrapToken(const Bytes& token, Bytes& out, std::string* err) {
    DATA_BLOB in{static_cast<DWORD>(token.size()), const_cast<BYTE*>(token.data())};
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        if (err) *err = "the invite token couldn't be protected (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    out.assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool unwrapToken(const Bytes& wrapped, Bytes& out) {
    if (wrapped.empty()) return false;
    DATA_BLOB in{static_cast<DWORD>(wrapped.size()), const_cast<BYTE*>(wrapped.data())};
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return false;
    out.assign(plain.pbData, plain.pbData + plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return true;
}

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

std::string pct(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (unreserved(c)) out += static_cast<char>(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unpct(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return false;
        const int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

constexpr size_t kMaxLinkBytes = 4096;

// The query of "<prefix>?k=v&...": each key once, values decoded. Whitespace around the pasted text is
// trimmed; a link over 4 KiB is refused before it is read. False on a repeated key or bad escape.
bool queryOf(const std::string& raw, const char* prefix, std::vector<std::pair<std::string, std::string>>& out) {
    const char* const space = " \t\r\n";
    const size_t first = raw.find_first_not_of(space);
    if (first == std::string::npos) return false;
    const std::string uri = raw.substr(first, raw.find_last_not_of(space) - first + 1);
    if (uri.size() > kMaxLinkBytes) return false;
    const std::string p = std::string(prefix) + "?";
    if (uri.compare(0, p.size(), p) != 0) return false;
    const std::string q = uri.substr(p.size());
    size_t start = 0;
    while (start <= q.size()) {
        size_t end = q.find('&', start);
        if (end == std::string::npos) end = q.size();
        const std::string part = q.substr(start, end - start);
        if (!part.empty()) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos) return false;
            std::string k, v;
            if (!unpct(part.substr(0, eq), k) || !unpct(part.substr(eq + 1), v)) return false;
            for (const auto& kv : out)
                if (kv.first == k) return false;
            out.emplace_back(k, v);
        }
        start = end + 1;
    }
    return true;
}

const std::string* valueOf(const std::vector<std::pair<std::string, std::string>>& q, const char* key) {
    for (const auto& kv : q)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

bool digitsU64(const std::string& s, uint64_t max, uint64_t& out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (max - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// The keys both links share: v=3, s, fp (32 bytes), h, p (1..65535).
bool commonKeys(const std::vector<std::pair<std::string, std::string>>& q, std::string& s, Bytes& fp,
                std::string& h, uint16_t& port) {
    const std::string *v = valueOf(q, "v"), *sv = valueOf(q, "s"), *fpv = valueOf(q, "fp"), *hv = valueOf(q, "h"),
                      *pv = valueOf(q, "p");
    if (!v || *v != "3" || !sv || sv->empty() || !fpv || !hv || hv->empty() || !pv) return false;
    if (!fromBase64URL(*fpv, fp) || fp.size() != 32) return false;
    uint64_t p = 0;
    if (!digitsU64(*pv, 65535, p) || p == 0) return false;
    s = *sv;
    h = *hv;
    port = static_cast<uint16_t>(p);
    return true;
}

const char* stateName(int s) {
    static const char* names[] = {"none", "open", "used", "burned", "closed"};
    return names[s];
}

} // namespace

// --- links ---

std::string InviteLink::toURI() const {
    return "kv2://invite?v=3&s=" + pct(serviceId) + "&fp=" + base64URL(hostCertSHA256) + "&tok=" + base64URL(token) +
           "&h=" + pct(host) + "&p=" + std::to_string(port) + "&lh=" + pct(lanHost) + "&exp=" + std::to_string(expUnix) +
           "&r=" + petRoleString(role);
}

bool InviteLink::parse(const std::string& uri, InviteLink& out) {
    std::vector<std::pair<std::string, std::string>> q;
    if (!queryOf(uri, "kv2://invite", q)) return false;
    InviteLink l;
    if (!commonKeys(q, l.serviceId, l.hostCertSHA256, l.host, l.port)) return false;
    const std::string *tok = valueOf(q, "tok"), *lh = valueOf(q, "lh"), *exp = valueOf(q, "exp"), *r = valueOf(q, "r");
    if (!tok || !lh || !exp || !r) return false;
    if (!fromBase64URL(*tok, l.token) || l.token.size() != kInviteTokenBytes) return false;
    const auto lan = parseIp(*lh);
    if (!lan || lan->v6) return false;
    l.lanHost = *lh;
    if (!digitsU64(*exp, UINT64_MAX, l.expUnix)) return false;
    l.role = petRoleFromString(*r);
    if (l.role == PetRole::Unknown) return false;
    out = std::move(l);
    return true;
}

std::string EndpointLink::toURI() const {
    return "kv2://endpoint?v=3&s=" + pct(serviceId) + "&fp=" + base64URL(hostCertSHA256) + "&h=" + pct(host) +
           "&p=" + std::to_string(port);
}

bool EndpointLink::parse(const std::string& uri, EndpointLink& out) {
    std::vector<std::pair<std::string, std::string>> q;
    if (!queryOf(uri, "kv2://endpoint", q)) return false;
    EndpointLink l;
    if (!commonKeys(q, l.serviceId, l.hostCertSHA256, l.host, l.port)) return false;
    out = std::move(l);
    return true;
}

// --- the store ---

InviteStore::InviteStore(std::wstring stateDir) : dir_(std::move(stateDir)) {}

bool InviteStore::readable() const {
    std::lock_guard<std::mutex> lock(mu_);
    return readable_;
}

bool InviteStore::load(std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::wstring path = dir_ + L"\\" + kInviteFile;
    state_ = State::None;
    invite_ = {};
    unsigned long win32 = 0;
    const FileState fs = fileState(path, &win32);
    if (fs == FileState::Missing) {
        readable_ = true;
        unreadable_.clear();
        return true;
    }
    auto failClosed = [&](const std::string& why) {
        readable_ = false;
        unreadable_ = why;
        if (err) *err = why;
        return false;
    };
    std::string text;
    if (fs == FileState::Unknown) return failClosed("invite.json can't be checked (Win32 error " + std::to_string(win32) + ")");
    if (!readWholeFile(path, text)) return failClosed("invite.json can't be read");

    json::Value v;
    std::string perr, st, role;
    bool ok = json::parse(text, v, &perr) && v.type == json::Value::Type::Object && json::getString(v, "state", st);
    bool plain_token = false;   // a file from before tokens were wrapped: written again, wrapped, once loaded
    bool cant_unwrap = false;   // wrapped for another user or PC, or damaged: no invite (closed)
    if (ok && st != "none" && st != "closed") {
        if (v.get("tokenProtected")) {
            Bytes wrapped;
            ok = json::getBytes(v, "tokenProtected", wrapped);
            if (ok && !unwrapToken(wrapped, invite_.token)) {
                cant_unwrap = true;
                invite_.token.assign(kInviteTokenBytes, 0); // checked below as any token is, never served
            }
        } else {
            ok = json::getBytes(v, "token", invite_.token);
            plain_token = ok;
        }
        ok = ok && invite_.token.size() == kInviteTokenBytes &&
             json::getString(v, "role", role) && json::getU64(v, "createdMs", invite_.createdMs) &&
             json::getU64(v, "expiresMs", invite_.expiresMs) && invite_.expiresMs > invite_.createdMs &&
             invite_.expiresMs - invite_.createdMs <= kInviteLifetimeMs;
        invite_.role = petRoleFromString(role);
        ok = ok && invite_.role != PetRole::Unknown;
    }
    if (ok) {
        wrong_ = 0; // counted in memory: an invite found at start begins again
        if (st == "open") state_ = State::Open;
        else if (st == "used") state_ = State::Used;
        else if (st == "burned") state_ = State::Burned;
        else if (st == "closed" || st == "none") state_ = State::Closed;
        else ok = false;
    }
    if (ok && cant_unwrap) {
        // Not this user's (or not whole): it reads as closed, so the PC can make a new one over it.
        state_ = State::Closed;
        invite_ = {};
        invite_.serial = ++serial_;
        readable_ = true;
        unreadable_.clear();
        if (err) *err = "invite.json's token can't be unwrapped for this Windows user, so it reads as closed";
        return false;
    }
    if (ok) {
        invite_.serial = ++serial_;
        readable_ = true;
        unreadable_.clear();
        if (plain_token && state_ != State::Closed) {
            // Best effort: a failed write leaves the old file as it was, and the next save wraps it.
            std::string ignored;
            saveLocked(&ignored);
        }
        return true;
    }
    // Read but not parsed: no invite is redeemable. Once a copy is kept for a human, a new invite may be
    // written over it; until then nothing is.
    state_ = State::None;
    invite_ = {};
    std::string what;
    if (keepBadCopy(dir_, kInviteFile, &what)) {
        readable_ = true;
        unreadable_.clear();
        if (err) *err = "invite.json didn't parse; " + what + ", and no invite is open";
        return false;
    }
    return failClosed("invite.json didn't parse, and " + what);
}

void InviteStore::saveFailedLocked(const std::string& why) {
    // Fail closed: serve no invite until invite.json can be read again, and take away the file that still
    // says open, so a restart can't bring a closed or burned invite back.
    readable_ = false;
    unreadable_ = "invite.json couldn't be saved (" + why + ")";
    DeleteFileW((dir_ + L"\\" + kInviteFile).c_str());
}

bool InviteStore::saveLocked(std::string* err) {
    json::ObjectWriter w;
    w.field("version", 2).field("state", stateName(static_cast<int>(state_)));
    if (state_ != State::None && state_ != State::Closed) {
        Bytes wrapped;
        if (!wrapToken(invite_.token, wrapped, err)) return false; // the caller fails closed, as for any failed save
        w.bytes("tokenProtected", wrapped)
            .field("role", petRoleString(invite_.role))
            .field("createdMs", invite_.createdMs)
            .field("expiresMs", invite_.expiresMs);
    }
    return writeOwnerOnlyFileAtomically(dir_, kInviteFile, w.finish() + "\n", err);
}

bool InviteStore::create(PetRole role, uint64_t nowMs, Invite& out, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!readable_) {
        if (err) *err = "Can't make an invite until invite.json can be read: " + unreadable_;
        return false;
    }
    if (role != PetRole::Owner && role != PetRole::Viewer) {
        if (err) *err = "An invite is for an owner or a viewer.";
        return false;
    }
    Invite inv;
    inv.token.resize(kInviteTokenBytes);
    if (!randomBytes(inv.token.data(), inv.token.size())) {
        if (err) *err = "No random bytes for the invite token.";
        return false;
    }
    inv.role = role;
    inv.createdMs = nowMs;
    inv.expiresMs = nowMs + kInviteLifetimeMs;
    inv.serial = ++serial_;
    invite_ = inv;
    state_ = State::Open;
    wrong_ = 0;
    std::string why;
    if (!saveLocked(&why)) {
        // Not shown, never redeemable: closed, and no invite is served until the file can be written.
        state_ = State::Closed;
        saveFailedLocked(why);
        if (err) *err = unreadable_;
        return false;
    }
    out = inv;
    return true;
}

std::optional<Invite> InviteStore::open(uint64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mu_);
    // A clock gone back before the invite was made opens nothing.
    if (!readable_ || state_ != State::Open || nowMs < invite_.createdMs || nowMs >= invite_.expiresMs)
        return std::nullopt;
    return invite_;
}

InviteStore::Fate InviteStore::fateOf(uint64_t serial, uint64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!readable_ || serial == 0 || serial != invite_.serial) return Fate::Closed; // a newer one replaced it
    if (state_ == State::None || state_ == State::Closed) return Fate::Closed;
    if (state_ == State::Open && nowMs >= invite_.createdMs && nowMs < invite_.expiresMs) return Fate::Open;
    return Fate::Ended; // used, burned, or ran out (or a clock gone back before it was made)
}

void InviteStore::close(std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::Open) return;
    state_ = State::Closed;
    std::string why;
    if (readable_ && !saveLocked(&why)) {
        saveFailedLocked(why);
        if (err) *err = unreadable_;
    }
}

RedeemResult InviteStore::redeem(uint64_t nowMs, const Bytes& hostCertSHA256, const Bytes& clientCertSHA256,
                                 bool leafMatches, const Bytes& clientProof, Invite* used, std::string* err, bool* burned) {
    std::lock_guard<std::mutex> lock(mu_);
    if (burned) *burned = false;
    if (!readable_ || state_ == State::None || state_ == State::Closed) return RedeemResult::NoInvite;
    // A clock gone back before the invite was made redeems nothing and burns nothing.
    if (state_ != State::Open || nowMs < invite_.createdMs || nowMs >= invite_.expiresMs)
        return RedeemResult::ExpiredOrUsed;
    // A wrong attempt is counted; the kInviteWrongAttemptsToBurn-th burns the invite.
    auto wrongAttempt = [&]() {
        if (++wrong_ < kInviteWrongAttemptsToBurn) return;
        state_ = State::Burned;
        if (burned) *burned = true;
        std::string why;
        if (!saveLocked(&why)) {
            saveFailedLocked(why);
            if (err) *err = unreadable_;
        }
    };
    if (!leafMatches) {
        wrongAttempt();
        return RedeemResult::CertMismatch;
    }
    const Bytes expected = computePairingProof(invite_.token, hostCertSHA256, clientCertSHA256, "kv2-pair-client");
    if (!constantTimeEqual(expected, clientProof)) {
        wrongAttempt();
        return RedeemResult::WrongProof;
    }
    state_ = State::Used;
    std::string why;
    if (!saveLocked(&why)) {
        // The use must be on disk before the device is added, or a restart could open it again.
        state_ = State::Closed;
        saveFailedLocked(why);
        if (err) *err = unreadable_;
        return RedeemResult::NotSaved;
    }
    if (used) *used = invite_;
    return RedeemResult::Accepted;
}

} // namespace kvhost::outside
