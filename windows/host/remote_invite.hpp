// Outside the home: remote invites and endpoint links (protocol v3 §8.4, pet-camera.md §3).
// An invite is open for 10 minutes, works once, and one is open at a time; a wrong proof or a certificate
// mismatch burns it. The store keeps it in invite.json in the host state folder, owner-only and written
// atomically, and fails closed: a file that is there but can't be read or parsed means no invite can be
// redeemed, and none is written over it until a copy is kept (state_files.hpp keepBadCopy). The token is written
// wrapped with DPAPI for this Windows user ("tokenProtected"); one that can't be unwrapped reads as a closed invite,
// and a file from before ("token", plain) is read once and written again wrapped.
#pragma once

#include "pet_role.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kvhost::outside {

using Bytes = std::vector<uint8_t>;

constexpr uint64_t kInviteLifetimeMs = 10ull * 60 * 1000;
constexpr size_t kInviteTokenBytes = 16; // 128 bits
// A wrong proof (or a TLS leaf that isn't clientCertDER) doesn't burn an open invite at once: with a 128-bit token
// a guess is hopeless, while burning on the first one let anyone who can reach the outside port spoil an invite.
// Each is a failure for its source in the limiter (v3 §8.5); the invite burns at the tenth. Counted in memory, for
// the invite open now.
constexpr int kInviteWrongAttemptsToBurn = 10;
constexpr const wchar_t* kInviteFile = L"invite.json";

// kv2://invite?v=3&s=..&fp=..&tok=..&h=..&p=..&lh=..&exp=..&r=..  (keys in this order; parsers read by name)
struct InviteLink {
    std::string serviceId;
    Bytes hostCertSHA256; // 32 bytes
    Bytes token;          // 16 bytes
    std::string host;     // outsideEndpoint.host
    uint16_t port = 0;    // the outside port
    std::string lanHost;  // the LAN IPv4
    uint64_t expUnix = 0; // unix seconds
    PetRole role = PetRole::Viewer;
    std::string toURI() const;
    // Every key present and well formed, unknown keys ignored, v=3. No expiry check (that is the caller's).
    static bool parse(const std::string& uri, InviteLink& out);
};

// kv2://endpoint?v=3&s=..&fp=..&h=..&p=..  (no secret)
struct EndpointLink {
    std::string serviceId;
    Bytes hostCertSHA256;
    std::string host;
    uint16_t port = 0;
    std::string toURI() const;
    static bool parse(const std::string& uri, EndpointLink& out);
};

// The invite as the store keeps it.
struct Invite {
    Bytes token;
    PetRole role = PetRole::Viewer;
    uint64_t createdMs = 0; // unix ms
    uint64_t expiresMs = 0; // unix ms
    // Which invite this is, in this run only (never saved): a new one for every invite made or loaded, so a
    // handshake admitted while one invite was open can tell it from any invite open later (v3 §8.7).
    uint64_t serial = 0;
};

enum class RedeemResult {
    Accepted,     // add the device with invite.role; the invite is now used
    NoInvite,     // none open (never made, closed, or the store can't be read): closed, counted as a failure
    ExpiredOrUsed,// accepted: false with the "expired or used" sentence (v3 §9)
    WrongProof,   // counted (the tenth burns it: see redeem's burned); accepted: false with its sentence (v3 §9)
    CertMismatch, // counted the same way; closed without a reply
    NotSaved,     // the invite was right but its use couldn't be saved: refused, and closed in memory
};

class InviteStore {
public:
    // stateDir: the host state folder. nowMs: unix ms (injected for tests).
    explicit InviteStore(std::wstring stateDir);

    // Reads invite.json. Missing: no invite. There but unreadable or unparsable: fail closed (readable() is
    // false and err says why). A file that was read but didn't parse is kept as invite.json.bad. A token that
    // can't be unwrapped: false with err, the invite reads as closed, and a new one may be made over it.
    bool load(std::string* err = nullptr);
    bool readable() const;

    // Makes a new invite, closing any open one. Refused while the file can't be read (the PC shows err).
    bool create(PetRole role, uint64_t nowMs, Invite& out, std::string* err = nullptr);
    // The open invite: made, not expired at nowMs, not used, burned or closed.
    std::optional<Invite> open(uint64_t nowMs) const;
    // What became of the invite whose serial is `serial` (Invite::serial), for a connection admitted while it was
    // open (v3 §8.7). Open: it is still the open invite. Closed: the PC closed it (cancelled, closed with the
    // endpoint or outside listening, a save that failed), or made a newer one in its place, or the store can't be
    // read. Ended: it ran out, or was used or burned (a redemption gets redeem's answer for that).
    enum class Fate { Open, Closed, Ended };
    Fate fateOf(uint64_t serial, uint64_t nowMs) const;
    // Closes it (outside viewing off, the address changed). A save that fails marks the store unreadable
    // (no invite is served) and removes invite.json, so the invite can't come back after a restart.
    void close(std::string* err = nullptr);

    // §8.4 step 4, under the caller's server lock or this store's own. leafMatches: the TLS leaf equals
    // clientCertDER. clientProof: the request's clientProofHMAC. A wrong proof or a mismatch is counted, and the
    // kInviteWrongAttemptsToBurn-th burns the invite: burned says whether this one did.
    RedeemResult redeem(uint64_t nowMs, const Bytes& hostCertSHA256, const Bytes& clientCertSHA256, bool leafMatches,
                        const Bytes& clientProof, Invite* used = nullptr, std::string* err = nullptr,
                        bool* burned = nullptr);

private:
    enum class State { None, Open, Used, Burned, Closed };
    bool saveLocked(std::string* err);
    void saveFailedLocked(const std::string& why);
    std::wstring dir_;
    mutable std::mutex mu_;
    bool readable_ = true;
    std::string unreadable_;
    State state_ = State::None;
    Invite invite_;
    int wrong_ = 0; // wrong proofs and mismatches against the invite open now (a new or loaded one starts at 0)
    uint64_t serial_ = 0; // the last Invite::serial given
};

} // namespace kvhost::outside
