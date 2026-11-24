// W8 Phase A: remote invites (v3 §8.4): the kv2://invite and kv2://endpoint links, the methodRaw 2 golden
// proof, and the invite store (10 minutes, once, one at a time, a wrong proof or a certificate mismatch burns
// it), kept in invite.json and failing closed. New temp folders only.
#include "kv_test.hpp"
#include "outside_test_dir.hpp"

#include "crypto_identity.hpp"
#include "json_lite.hpp"
#include "remote_invite.hpp"
#include "state_files.hpp"

using namespace kvhost::outside;
using kvtest::OutsideTempState;

namespace {

Bytes seq(uint8_t from, size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(from + i);
    return b;
}

Bytes unhex(const char* h) {
    Bytes b;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
        auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        b.push_back(static_cast<uint8_t>(v(h[i]) * 16 + v(h[i + 1])));
    }
    return b;
}

const Bytes kHost = seq(0x20, 32);
const Bytes kClient = seq(0x40, 32);
constexpr uint64_t kNow = 1'763'500'000'000ull; // unix ms

Bytes proofFor(const Bytes& token) { return kvhost::computePairingProof(token, kHost, kClient, "kv2-pair-client"); }

InviteLink sampleLink() {
    InviteLink l;
    l.serviceId = "3F2A9C1B-0D4E-4A5B-9C8D-7E6F5A4B3C2D";
    l.hostCertSHA256 = kHost;
    l.token = seq(0, 16);
    l.host = "203.0.113.160";
    l.port = 41234;
    l.lanHost = "192.168.1.20";
    l.expUnix = 1'763'500'600;
    l.role = kvhost::PetRole::Viewer;
    return l;
}

} // namespace

KV_TEST(the_methodraw_2_proof_matches_its_golden_vector) {
    // secret = the 16 token bytes 00..0f, hostCertSHA256 = 20..3f, clientCertSHA256 = 40..5f; derived
    // separately (Python hmac/hashlib HKDF-SHA256, info "kv2-pair-v1").
    const Bytes tok = seq(0, 16);
    KV_CHECK(proofFor(tok) == unhex("78008978cc1c37f62a23e16556b9310784147ea971fd751536c466af77ae68bf"));
    KV_CHECK(kvhost::computePairingProof(tok, kHost, kClient, "kv2-pair-server") ==
             unhex("efff057cf87372270db5397994830b35994985429daf828cf54023c616ee18b1"));
}

KV_TEST(an_invite_link_round_trips_with_its_keys_in_order) {
    const InviteLink l = sampleLink();
    const std::string uri = l.toURI();
    KV_CHECK(uri.rfind("kv2://invite?v=3&s=", 0) == 0);
    const size_t s = uri.find("&s="), fp = uri.find("&fp="), tok = uri.find("&tok="), h = uri.find("&h="),
                 p = uri.find("&p="), lh = uri.find("&lh="), exp = uri.find("&exp="), r = uri.find("&r=");
    KV_CHECK(s < fp && fp < tok && tok < h && h < p && p < lh && lh < exp && exp < r);
    KV_CHECK(uri.find('=') != std::string::npos && uri.find("==") == std::string::npos); // base64url, no padding
    KV_CHECK(uri.rfind("kv2://pair", 0) != 0);                                          // old scanners ignore it
    InviteLink back;
    KV_CHECK(InviteLink::parse(uri, back));
    KV_CHECK(back.serviceId == l.serviceId && back.hostCertSHA256 == l.hostCertSHA256 && back.token == l.token);
    KV_CHECK(back.host == l.host && back.port == l.port && back.lanHost == l.lanHost && back.expUnix == l.expUnix);
    KV_CHECK(back.role == kvhost::PetRole::Viewer);
    // Unknown keys are ignored.
    KV_CHECK(InviteLink::parse(uri + "&later=1", back));
}

KV_TEST(a_malformed_invite_link_is_refused) {
    const std::string good = sampleLink().toURI();
    auto without = [&](const char* key) {
        const std::string k = std::string("&") + key + "=";
        const size_t at = good.find(k);
        const size_t end = good.find('&', at + 1);
        return good.substr(0, at) + (end == std::string::npos ? "" : good.substr(end));
    };
    InviteLink out;
    for (const char* key : {"s", "fp", "tok", "h", "p", "lh", "exp", "r"}) KV_CHECK(!InviteLink::parse(without(key), out));
    KV_CHECK(!InviteLink::parse("kv2://endpoint" + good.substr(std::string("kv2://invite").size()), out));
    InviteLink shortTok = sampleLink();
    shortTok.token = seq(0, 15);
    KV_CHECK(!InviteLink::parse(shortTok.toURI(), out));
    InviteLink badRole = sampleLink();
    badRole.role = kvhost::PetRole::Unknown;
    KV_CHECK(!InviteLink::parse(badRole.toURI(), out));
    InviteLink zeroPort = sampleLink();
    zeroPort.port = 0;
    KV_CHECK(!InviteLink::parse(zeroPort.toURI(), out));
    InviteLink v6Lan = sampleLink();
    v6Lan.lanHost = "fe80::1";
    KV_CHECK(!InviteLink::parse(v6Lan.toURI(), out));
    std::string v2 = good;
    v2.replace(v2.find("v=3"), 3, "v=2");
    KV_CHECK(!InviteLink::parse(v2, out));
    KV_CHECK(!InviteLink::parse(good + "&tok=AAAA", out)); // a key twice
    KV_CHECK(!InviteLink::parse(good + "&x=%G1", out));    // a bad escape
}

KV_TEST(an_endpoint_link_carries_no_secret) {
    EndpointLink e;
    e.serviceId = "3F2A9C1B-0D4E-4A5B-9C8D-7E6F5A4B3C2D";
    e.hostCertSHA256 = kHost;
    e.host = "203.0.113.161";
    e.port = 41234;
    const std::string uri = e.toURI();
    KV_CHECK(uri.rfind("kv2://endpoint?v=3&s=", 0) == 0);
    KV_CHECK(uri.find("tok=") == std::string::npos && uri.find("lh=") == std::string::npos);
    EndpointLink back;
    KV_CHECK(EndpointLink::parse(uri, back));
    KV_CHECK(back.serviceId == e.serviceId && back.hostCertSHA256 == kHost && back.host == e.host && back.port == e.port);
    InviteLink notAnInvite;
    KV_CHECK(!InviteLink::parse(uri, notAnInvite));
}

KV_TEST(an_invite_is_open_ten_minutes_with_a_128_bit_token) {
    OutsideTempState t(L"invite-open");
    KV_CHECK(t.fresh);
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    KV_CHECK(!store.open(kNow).has_value());
    Invite a, b;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, a));
    KV_CHECK(a.token.size() == 16 && a.expiresMs == kNow + 600'000);
    KV_CHECK(store.open(kNow + 599'999).has_value());
    KV_CHECK(!store.open(kNow + 600'000).has_value());
    // One at a time: a new invite closes the old one, and the tokens differ.
    KV_CHECK(store.create(kvhost::PetRole::Owner, kNow + 1, b));
    KV_CHECK(a.token != b.token);
    KV_CHECK(store.open(kNow + 2)->token == b.token);
    KV_CHECK(store.redeem(kNow + 2, kHost, kClient, true, proofFor(a.token)) == RedeemResult::WrongProof);
}

KV_TEST(an_invite_works_once) {
    OutsideTempState t(L"invite-once");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv, used;
    KV_CHECK(store.create(kvhost::PetRole::Owner, kNow, inv));
    KV_CHECK(store.redeem(kNow + 1000, kHost, kClient, true, proofFor(inv.token), &used) == RedeemResult::Accepted);
    KV_CHECK(used.role == kvhost::PetRole::Owner);
    KV_CHECK(!store.open(kNow + 1000).has_value());
    KV_CHECK(store.redeem(kNow + 2000, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::ExpiredOrUsed);
    // And stays used after a restart.
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    KV_CHECK(!again.open(kNow + 3000).has_value());
    KV_CHECK(again.redeem(kNow + 3000, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::ExpiredOrUsed);
}

KV_TEST(an_expired_invite_fails) {
    OutsideTempState t(L"invite-expired");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    KV_CHECK(store.redeem(kNow + 600'000, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::ExpiredOrUsed);
}

// A wrong proof is counted, not burned at once (with a 128-bit token a guess is hopeless, and burning on the first
// let anyone who can reach the port spoil an invite); the tenth burns it.
KV_TEST(a_wrong_proof_is_counted_and_the_tenth_burns_the_invite) {
    OutsideTempState t(L"invite-wrong");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    KV_CHECK(kInviteWrongAttemptsToBurn == 10); // the report's "about ten"
    Bytes wrong = proofFor(inv.token);
    wrong[0] ^= 1;
    for (int i = 1; i < 10; ++i) { // nine, whatever the constant says: the first alone burned it before
        bool burned = true;
        KV_CHECK(store.redeem(kNow + i, kHost, kClient, true, wrong, nullptr, nullptr, &burned) == RedeemResult::WrongProof);
        KV_CHECK_MSG(!burned && store.open(kNow + i).has_value(), "burned after " + std::to_string(i));
    }
    // Nine wrong ones spoil nothing: the right proof still redeems it.
    KV_CHECK(store.redeem(kNow + 20, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::Accepted);
    // A new invite starts its own count; the tenth wrong proof burns it, and it stays burned after a restart.
    Invite next;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 30, next));
    Bytes wrong2 = proofFor(next.token);
    wrong2[0] ^= 1;
    bool burned = false;
    for (int i = 1; i <= kInviteWrongAttemptsToBurn; ++i) {
        KV_CHECK(store.redeem(kNow + 30 + i, kHost, kClient, true, wrong2, nullptr, nullptr, &burned) == RedeemResult::WrongProof);
    }
    KV_CHECK(burned && !store.open(kNow + 50).has_value());
    KV_CHECK(store.redeem(kNow + 51, kHost, kClient, true, proofFor(next.token)) == RedeemResult::ExpiredOrUsed);
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    KV_CHECK(!again.open(kNow + 52).has_value());
}

KV_TEST(a_certificate_mismatch_is_counted_with_the_wrong_proofs) {
    OutsideTempState t(L"invite-mismatch");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    Bytes wrong = proofFor(inv.token);
    wrong[0] ^= 1;
    // The right proof does not save a connection whose TLS leaf isn't clientCertDER; it is counted, as a wrong proof is.
    KV_CHECK(store.redeem(kNow + 1, kHost, kClient, false, proofFor(inv.token)) == RedeemResult::CertMismatch);
    KV_CHECK(store.open(kNow + 2).has_value());
    bool burned = false;
    for (int i = 2; i < kInviteWrongAttemptsToBurn; ++i) {
        KV_CHECK(store.redeem(kNow + i, kHost, kClient, true, wrong, nullptr, nullptr, &burned) == RedeemResult::WrongProof);
    }
    KV_CHECK(!burned && store.open(kNow + 20).has_value()); // nine so far, of both kinds
    KV_CHECK(store.redeem(kNow + 21, kHost, kClient, false, proofFor(inv.token), nullptr, nullptr, &burned) ==
             RedeemResult::CertMismatch);
    KV_CHECK(burned && !store.open(kNow + 22).has_value()); // the tenth, a mismatch, burns it
    KV_CHECK(store.redeem(kNow + 23, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::ExpiredOrUsed);
}

KV_TEST(with_no_invite_nothing_is_redeemed) {
    OutsideTempState t(L"invite-none");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    KV_CHECK(store.redeem(kNow, kHost, kClient, true, proofFor(seq(0, 16))) == RedeemResult::NoInvite);
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    store.close();
    KV_CHECK(!store.open(kNow + 1).has_value());
    KV_CHECK(store.redeem(kNow + 1, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::NoInvite);
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    KV_CHECK(!again.open(kNow + 2).has_value());
}

KV_TEST(an_open_invite_survives_a_restart) {
    OutsideTempState t(L"invite-restart");
    Invite inv;
    {
        InviteStore store(t.dir);
        KV_CHECK(store.load());
        KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    }
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    const auto open = again.open(kNow + 1);
    KV_CHECK(open.has_value() && open->token == inv.token && open->role == kvhost::PetRole::Viewer);
    KV_CHECK(again.redeem(kNow + 2, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::Accepted);
}

namespace {

std::string readText(const std::wstring& path) {
    std::string s;
    kvhost::readWholeFile(path, s);
    return s;
}

// The file with its tokenProtected value replaced.
std::string withWrapped(const std::string& text, const std::string& value) {
    const std::string key = "\"tokenProtected\":\"";
    const size_t at = text.find(key);
    if (at == std::string::npos) return text;
    const size_t from = at + key.size();
    const size_t to = text.find('"', from);
    return text.substr(0, from) + value + text.substr(to);
}

} // namespace

// The token is never on disk as it is: invite.json holds it wrapped with DPAPI for this Windows user, and the
// store reads it back.
KV_TEST(an_invites_token_is_written_wrapped_never_plain) {
    OutsideTempState t(L"invite-wrapped");
    Invite inv;
    {
        InviteStore store(t.dir);
        KV_CHECK(store.load());
        KV_CHECK(store.create(kvhost::PetRole::Owner, kNow, inv));
    }
    const std::string text = readText(t.file(kInviteFile));
    KV_CHECK(text.find("\"tokenProtected\":\"") != std::string::npos);
    KV_CHECK_MSG(text.find("\"token\":") == std::string::npos, text);
    KV_CHECK(text.find(kvhost::base64Encode(inv.token.data(), inv.token.size())) == std::string::npos);
    KV_CHECK(text.find(std::string(inv.token.begin(), inv.token.end())) == std::string::npos);
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    const auto open = again.open(kNow + 1);
    KV_CHECK(open.has_value() && open->token == inv.token && open->role == kvhost::PetRole::Owner);
}

// A token that can't be unwrapped (wrapped for another Windows user or PC, or damaged) reads as a closed invite:
// nothing is redeemable, the store can still be written, and the PC makes a new one.
KV_TEST(a_token_that_cant_be_unwrapped_reads_as_a_closed_invite) {
    OutsideTempState t(L"invite-unwrap");
    Invite inv;
    {
        InviteStore store(t.dir);
        KV_CHECK(store.load());
        KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    }
    const std::string text = readText(t.file(kInviteFile));
    KV_REQUIRE(kvtest::writeRaw(t.file(kInviteFile), withWrapped(text, kvhost::base64Encode(seq(1, 200).data(), 200))));
    InviteStore store(t.dir);
    std::string err;
    KV_CHECK(!store.load(&err));
    KV_CHECK_MSG(err.find("can't be unwrapped") != std::string::npos, err);
    KV_CHECK(store.readable());
    KV_CHECK(!store.open(kNow + 1).has_value());
    KV_CHECK(store.redeem(kNow + 2, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::NoInvite);
    KV_CHECK(!kvhost::fileExists(t.file(L"invite.json.bad"))); // not a damaged file: nothing to keep
    Invite next;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 3, next));
    KV_CHECK(store.open(kNow + 4).has_value());
}

// A file from before tokens were wrapped is read, and written again wrapped at once, so the plain token is gone.
KV_TEST(a_plain_token_from_before_is_read_and_written_again_wrapped) {
    OutsideTempState t(L"invite-plain");
    const Bytes token = seq(0x10, 16);
    const std::string tok = kvhost::base64Encode(token.data(), token.size());
    KV_REQUIRE(kvtest::writeRaw(t.file(kInviteFile),
                                "{\"version\":1,\"state\":\"open\",\"token\":\"" + tok + "\",\"role\":\"viewer\",\"createdMs\":" +
                                    std::to_string(kNow) + ",\"expiresMs\":" + std::to_string(kNow + kInviteLifetimeMs) + "}\n"));
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    const auto open = store.open(kNow + 1);
    KV_CHECK(open.has_value() && open->token == token);
    const std::string text = readText(t.file(kInviteFile));
    KV_CHECK(text.find("\"tokenProtected\":\"") != std::string::npos);
    KV_CHECK_MSG(text.find(tok) == std::string::npos, "the plain token is still in invite.json");
    KV_CHECK(store.redeem(kNow + 2, kHost, kClient, true, proofFor(token)) == RedeemResult::Accepted);
}

KV_TEST(an_invite_file_that_doesnt_parse_fails_closed_and_is_kept) {
    OutsideTempState t(L"invite-bad");
    KV_CHECK(kvtest::writeRaw(t.file(kInviteFile), "{\"state\":\"open\",\"token\":"));
    InviteStore store(t.dir);
    std::string err;
    KV_CHECK(!store.load(&err));
    KV_CHECK(!err.empty());
    KV_CHECK(kvhost::fileExists(t.file(L"invite.json.bad")));
    KV_CHECK(!store.open(kNow).has_value());
    KV_CHECK(store.redeem(kNow, kHost, kClient, true, proofFor(seq(0, 16))) == RedeemResult::NoInvite);
    // The copy is kept, so a new invite may be written over it.
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
}

KV_TEST(an_invite_that_says_more_than_ten_minutes_is_not_trusted) {
    OutsideTempState t(L"invite-long");
    const std::string tok = kvhost::base64Encode(seq(0, 16).data(), 16);
    KV_CHECK(kvtest::writeRaw(t.file(kInviteFile),
                              "{\"version\":1,\"state\":\"open\",\"token\":\"" + tok +
                                  "\",\"role\":\"owner\",\"createdMs\":1000,\"expiresMs\":99999999999999}\n"));
    InviteStore store(t.dir);
    KV_CHECK(!store.load());
    KV_CHECK(!store.open(2000).has_value());
}

KV_TEST(an_invite_file_that_cant_be_read_fails_closed) {
    OutsideTempState t(L"invite-unreadable");
    // A folder where the file should be: it is there, but can't be read.
    KV_CHECK(CreateDirectoryW(t.file(kInviteFile).c_str(), nullptr) != 0);
    InviteStore store(t.dir);
    std::string err;
    KV_CHECK(!store.load(&err));
    KV_CHECK(!store.readable());
    KV_CHECK(store.redeem(kNow, kHost, kClient, true, proofFor(seq(0, 16))) == RedeemResult::NoInvite);
    Invite inv;
    std::string why;
    KV_CHECK(!store.create(kvhost::PetRole::Viewer, kNow, inv, &why));
    KV_CHECK(why.find("Can't make an invite") != std::string::npos);
    KV_CHECK(!kvhost::fileExists(t.file(L"invite.json.bad")));
}

KV_TEST(a_burn_that_cant_be_saved_serves_no_invites_and_doesnt_come_back) {
    // A failed save marks the store unreadable (no invite is served), and the invite
    // can't come back after a restart.
    OutsideTempState t(L"invite-savefail");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    // invite.json.tmp as a folder: the next atomic write fails.
    KV_CHECK(CreateDirectoryW(t.file(L"invite.json.tmp").c_str(), nullptr) != 0);
    Bytes wrong = proofFor(inv.token);
    wrong[0] ^= 1;
    std::string err;
    // Wrong attempts before the tenth save nothing; the tenth burns it, and that save is what fails.
    for (int i = 1; i < kInviteWrongAttemptsToBurn; ++i) {
        KV_CHECK(store.redeem(kNow + i, kHost, kClient, true, wrong, nullptr, &err) == RedeemResult::WrongProof);
    }
    KV_CHECK(store.readable());
    KV_CHECK(store.redeem(kNow + 10, kHost, kClient, true, wrong, nullptr, &err) == RedeemResult::WrongProof);
    KV_CHECK(!store.readable());
    Invite another;
    KV_CHECK(!store.create(kvhost::PetRole::Viewer, kNow + 2, another));
    InviteStore again(t.dir);
    again.load();
    KV_CHECK(!again.open(kNow + 3).has_value());
    KV_CHECK(again.redeem(kNow + 3, kHost, kClient, true, proofFor(inv.token)) != RedeemResult::Accepted);
}

KV_TEST(a_close_that_cant_be_saved_doesnt_come_back) {
    OutsideTempState t(L"invite-closefail");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    KV_CHECK(CreateDirectoryW(t.file(L"invite.json.tmp").c_str(), nullptr) != 0);
    std::string err;
    store.close(&err);
    KV_CHECK(!err.empty());
    KV_CHECK(!store.readable());
    InviteStore again(t.dir);
    again.load();
    KV_CHECK(!again.open(kNow + 1).has_value());
}

KV_TEST(a_clock_gone_backwards_redeems_nothing) {
    OutsideTempState t(L"invite-clock");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    Invite inv;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, inv));
    KV_CHECK(!store.open(kNow - 1).has_value());
    KV_CHECK(store.redeem(kNow - 1, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::ExpiredOrUsed);
    // Not burned by it: on time again, it works.
    KV_CHECK(store.redeem(kNow + 1, kHost, kClient, true, proofFor(inv.token)) == RedeemResult::Accepted);
}

// What became of the invite a connection was admitted for. Closed
// only when the PC closed it (or made a newer one in its place); a use, a burn or running out is Ended.
KV_TEST(an_invites_fate_tells_a_close_by_the_pc_from_an_end) {
    OutsideTempState t(L"invite-fate");
    InviteStore store(t.dir);
    KV_CHECK(store.load());
    KV_CHECK(store.fateOf(0, kNow) == InviteStore::Fate::Closed); // never admitted for an invite
    Invite a, b, c, d;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow, a));
    KV_CHECK(a.serial != 0 && store.open(kNow)->serial == a.serial);
    KV_CHECK(store.fateOf(a.serial, kNow + 1) == InviteStore::Fate::Open);
    KV_CHECK(store.fateOf(a.serial, kNow + 600'000) == InviteStore::Fate::Ended); // ran out
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 2, b));                 // replaces a
    KV_CHECK(b.serial != a.serial);
    KV_CHECK(store.fateOf(a.serial, kNow + 3) == InviteStore::Fate::Closed);
    KV_CHECK(store.fateOf(b.serial, kNow + 3) == InviteStore::Fate::Open);
    store.close(); // cancelled
    KV_CHECK(store.fateOf(b.serial, kNow + 4) == InviteStore::Fate::Closed);
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 5, c));
    KV_CHECK(store.redeem(kNow + 6, kHost, kClient, true, proofFor(c.token)) == RedeemResult::Accepted);
    KV_CHECK(store.fateOf(c.serial, kNow + 7) == InviteStore::Fate::Ended); // used
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 8, d));
    Bytes wrong = proofFor(d.token);
    wrong[0] ^= 1;
    KV_CHECK(store.redeem(kNow + 9, kHost, kClient, true, wrong) == RedeemResult::WrongProof);
    KV_CHECK(store.fateOf(d.serial, kNow + 9) == InviteStore::Fate::Open); // one wrong proof is counted, not burned
    for (int i = 1; i < kInviteWrongAttemptsToBurn; ++i) store.redeem(kNow + 9, kHost, kClient, true, wrong);
    KV_CHECK(store.fateOf(d.serial, kNow + 10) == InviteStore::Fate::Ended); // the tenth burned it
    // An invite found open at start gets a serial of its own too.
    Invite e;
    KV_CHECK(store.create(kvhost::PetRole::Viewer, kNow + 11, e));
    InviteStore again(t.dir);
    KV_CHECK(again.load());
    const auto open = again.open(kNow + 12);
    KV_CHECK(open.has_value() && open->serial != 0 && again.fateOf(open->serial, kNow + 12) == InviteStore::Fate::Open);
}

KV_TEST(a_link_is_trimmed_and_capped_at_4_kib) {
    const std::string good = sampleLink().toURI();
    InviteLink out;
    KV_CHECK(InviteLink::parse("  \t" + good + "\r\n", out));
    KV_CHECK(!InviteLink::parse(good + "&pad=" + std::string(4096, 'a'), out));
    EndpointLink e;
    e.serviceId = "S";
    e.hostCertSHA256 = kHost;
    e.host = "203.0.113.161";
    e.port = 1;
    EndpointLink back;
    KV_CHECK(EndpointLink::parse(" " + e.toURI() + "\n", back));
    KV_CHECK(!EndpointLink::parse(e.toURI() + "&pad=" + std::string(4096, 'a'), back));
}
