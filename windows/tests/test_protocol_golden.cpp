// Byte-for-byte tests of the C++ wire protocol, crypto identity and pairing payload against golden
// vectors derived independently from the Swift sources (windows/tests/golden/derive_vectors.py).
#include "kv_test.hpp"

#include "crypto_identity.hpp"
#include "golden/golden_vectors.inc"
#include "host_state.hpp"
#include "json_lite.hpp"
#include "pairing_payload.hpp"
#include "state_files.hpp"
#include "wire_protocol.hpp"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h> // a wrap with other entropy, for a row

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace kvhost;

namespace {
Bytes B(const unsigned char* p, size_t n) { return Bytes(p, p + n); }
std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }
std::string hex(const Bytes& b) { return hexLower(b.data(), b.size()); }
} // namespace

// ---- WireProtocol.swift -----------------------------------------------------------------------

KV_TEST(pack_matches_swift_wirepacket) {
    const Bytes got = packFrame(MsgType::Pong, 0, std::string("{}"));
    KV_CHECK(got == B(kGoldenPackPong, kGoldenPackPong_len));
}

KV_TEST(video_access_unit_header_encode_and_decode) {
    VideoAccessUnitHeader h;
    h.streamEpoch = 0x01020304;
    h.frameSeq = 0x1122334455667788ULL;
    h.hostCaptureNs = 0x0102030405060708ULL;
    h.hostEncodeDoneNs = 0x0A0B0C0D0E0F1011ULL;
    h.flags = 0x03;
    const uint8_t avcc[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    Bytes out;
    h.encode(avcc, sizeof(avcc), out);
    KV_CHECK(out == B(kGoldenAuPayload, kGoldenAuPayload_len));

    VideoAccessUnitHeader d;
    const uint8_t* p = nullptr;
    size_t n = 0;
    KV_REQUIRE(VideoAccessUnitHeader::decode(out.data(), out.size(), d, &p, &n));
    KV_CHECK(d.streamEpoch == h.streamEpoch && d.frameSeq == h.frameSeq && d.hostCaptureNs == h.hostCaptureNs &&
             d.hostEncodeDoneNs == h.hostEncodeDoneNs && d.flags == h.flags);
    KV_CHECK(n == sizeof(avcc) && std::memcmp(p, avcc, n) == 0);
    KV_CHECK(d.isKeyframe() && d.hasClockOverlay());
    // Swift: guard data.count > binarySize
    KV_CHECK(!VideoAccessUnitHeader::decode(out.data(), VideoAccessUnitHeader::kBinarySize, d, &p, &n));
}

KV_TEST(frame_reader_rules) {
    FrameReader r;
    Bytes f1 = packFrame(MsgType::Ping, 7, std::string("{\"a\":1}"));
    Bytes f2 = packFrame(MsgType::SelectModeReq, 0, std::string("{}"));
    Bytes both = f1;
    both.insert(both.end(), f2.begin(), f2.end());
    MsgType t;
    uint8_t flags = 0;
    Bytes payload;
    r.append(both.data(), 5);
    KV_CHECK(r.next(t, flags, payload) == FrameReader::Result::NeedMore);
    r.append(both.data() + 5, both.size() - 5);
    KV_REQUIRE(r.next(t, flags, payload) == FrameReader::Result::Frame);
    KV_CHECK(t == MsgType::Ping && flags == 7 && S(payload) == "{\"a\":1}");
    KV_REQUIRE(r.next(t, flags, payload) == FrameReader::Result::Frame);
    KV_CHECK(t == MsgType::SelectModeReq && S(payload) == "{}");
    KV_CHECK(r.next(t, flags, payload) == FrameReader::Result::NeedMore);

    FrameReader bad;
    const uint8_t magic[] = {'X', 'V', 1, 0, 0, 0, 0, 0};
    bad.append(magic, sizeof(magic));
    KV_CHECK(bad.next(t, flags, payload) == FrameReader::Result::Violation);
    // A v1 peer's reader (an old phone) drops the connection on 0x07: this is why the host never
    // sends 0x07 before the phone said v2.
    FrameReader v1(1);
    const uint8_t ut[] = {'K', 'V', 0x07, 0, 0, 0, 0, 0};
    v1.append(ut, sizeof(ut));
    KV_CHECK(v1.next(t, flags, payload) == FrameReader::Result::Violation);
    // v2 knows 0x07-0x0A.
    for (uint8_t ty = 0x07; ty <= 0x0A; ++ty) {
        FrameReader v2;
        const uint8_t f[] = {'K', 'V', ty, 0, 0, 0, 0, 2, '{', '}'};
        v2.append(f, sizeof(f));
        KV_CHECK(v2.next(t, flags, payload) == FrameReader::Result::Frame && static_cast<uint8_t>(t) == ty && S(payload) == "{}");
    }
    // A type nobody knows yet: a violation for a strict reader (the pairing connection), skipped
    // whole by the stream connection's reader, which then reads the next frame. 0x7E, as the Swift
    // test uses: v3 took 0x0B-0x0F and 0x12-0x19 (protocol-v3-pet-camera.md §1 rule 7).
    const uint8_t future[] = {'K', 'V', 0x7E, 0, 0, 0, 0, 3, 'a', 'b', 'c'};
    FrameReader strict;
    strict.append(future, sizeof(future));
    KV_CHECK(strict.next(t, flags, payload) == FrameReader::Result::Violation);
    FrameReader skipping(kHostProtocolVersion, true);
    skipping.append(future, sizeof(future));
    const Bytes ping = packFrame(MsgType::Ping, 0, std::string("{}"));
    skipping.append(ping.data(), ping.size());
    KV_CHECK(skipping.next(t, flags, payload) == FrameReader::Result::Unknown && static_cast<uint8_t>(t) == 0x7E &&
             S(payload) == "abc");
    KV_CHECK(skipping.next(t, flags, payload) == FrameReader::Result::Frame && t == MsgType::Ping);
    // Skipping still checks the magic and the length.
    FrameReader skip_big(kHostProtocolVersion, true);
    const uint8_t fb[] = {'K', 'V', 0x7E, 0, 0x00, 0x40, 0x00, 0x01};
    skip_big.append(fb, sizeof(fb));
    KV_CHECK(skip_big.next(t, flags, payload) == FrameReader::Result::Violation);
    FrameReader big;
    const uint8_t bg[] = {'K', 'V', 0x03, 0, 0x00, 0x40, 0x00, 0x01}; // 4 MiB + 1
    big.append(bg, sizeof(bg));
    KV_CHECK(big.next(t, flags, payload) == FrameReader::Result::Violation);
}

KV_TEST(hello_json_matches_swift_encoder) {
    HelloServerMessage m;
    m.protocolVersion = 1;
    m.modeRaw = 3;
    m.sbsLayoutRaw = 1;
    m.allowPhoneModeSwitch = true;
    m.width = 1280;
    m.height = 424;
    m.codecRaw = 0;
    KV_CHECK(toJson(m) == kGoldenHelloJson);
    HelloServerMessage back;
    KV_CHECK(fromJson(std::string(kGoldenHelloJson), back) && back.width == 1280 && back.modeRaw == 3);
}

KV_TEST(stream_config_json_matches_swift_encoder_including_escaped_slashes) {
    StreamConfigMessage m;
    m.streamEpoch = 2;
    m.codecRaw = 0;
    m.width = 1024;
    m.height = 848;
    m.modeRaw = 0;
    m.sbsLayoutRaw = 0;
    m.nalLengthHeaderBytes = 4;
    m.parameterSets = {B(kGoldenSps, kGoldenSps_len), B(kGoldenPps, kGoldenPps_len)};
    const std::string got = toJson(m);
    KV_CHECK_MSG(got == kGoldenStreamConfigJson, got);
    StreamConfigMessage back;
    KV_REQUIRE(fromJson(got, back));
    KV_CHECK(back.parameterSets == m.parameterSets && back.streamEpoch == 2 && back.height == 848);
}

KV_TEST(pong_json_handles_full_uint64_range) {
    PongMessage m;
    m.clientSendNs = 123456789012345ULL;
    m.serverRecvNs = 18446744073709551615ULL;
    m.serverSendNs = 9223372036854775813ULL;
    KV_CHECK(toJson(m) == kGoldenPongJson);
}

KV_TEST(pair_response_json_matches_swift_encoder) {
    PairResponseMessage ok;
    ok.accepted = true;
    ok.serviceId = "a1b2c3d4e5f6";
    ok.streamPort = 50123;
    ok.serverProofHMAC = B(kGoldenProof32, kGoldenProof32_len);
    ok.message = "Paired";
    KV_CHECK(toJson(ok) == kGoldenPairResponseJson);

    PairResponseMessage reject;
    reject.message = "Invalid pairing code or token; pairing window closed.";
    KV_CHECK(toJson(reject) == kGoldenPairRejectJson);
    PairResponseMessage back;
    KV_CHECK(fromJson(std::string(kGoldenPairRejectJson), back) && !back.accepted && back.serverProofHMAC.empty());
}

KV_TEST(decodes_client_messages_in_any_key_order_with_whitespace_and_escapes) {
    PingMessage ping;
    KV_REQUIRE(fromJson(std::string("{ \"lastBarcodeLatencyUs\" : 4000000, \"clientSendNs\":18446744073709551615,"
                                    "\"lastDecodedSeq\":42,\"lastHostToDisplayLatencyUs\":0 }"),
                        ping));
    KV_CHECK(ping.clientSendNs == 18446744073709551615ULL && ping.lastDecodedSeq == 42 &&
             ping.lastBarcodeLatencyUs == 4000000u);
    PingMessage overflow;
    KV_CHECK(!fromJson(std::string("{\"clientSendNs\":1,\"lastDecodedSeq\":1,\"lastHostToDisplayLatencyUs\":4294967296,"
                                   "\"lastBarcodeLatencyUs\":0}"),
                       overflow));
    PingMessage missing;
    KV_CHECK(!fromJson(std::string("{\"clientSendNs\":1}"), missing));

    SelectModeMessage sel;
    KV_CHECK(fromJson(std::string("{\"sbsLayoutRaw\":2,\"modeRaw\":3}"), sel) && sel.modeRaw == 3 && sel.sbsLayoutRaw == 2);

    // Swift escapes '/' inside base64 strings as "\/".
    PairRequestMessage req;
    KV_REQUIRE(fromJson(std::string("{\"methodRaw\":1,\"clientCertDER\":\"MAMCAQA=\",\"clientProofHMAC\":\"\\/\\/8=\"}"), req));
    KV_CHECK(req.methodRaw == 1);
    KV_CHECK(req.clientCertDER == (Bytes{0x30, 0x03, 0x02, 0x01, 0x00}));
    KV_CHECK(req.clientProofHMAC == (Bytes{0xFF, 0xFF}));
    PairRequestMessage round;
    KV_CHECK(fromJson(toJson(req), round) && round.clientProofHMAC == req.clientProofHMAC);
}

// ---- CryptoIdentity.swift ---------------------------------------------------------------------

// ---- protocol v2 (shared/WireProtocol.swift) -----------------------------------------------------
// The expected JSON follows the same JSONEncoder rules as derive_vectors.py: compact, keys in
// declaration order, nil optionals left out (encodeIfPresent), a Float or Double that holds a whole
// number written without ".0".

KV_TEST(v2_ping_json_and_the_v1_path_reads_it) {
    PingMessage v2;
    v2.clientSendNs = 1;
    v2.lastDecodedSeq = 2;
    v2.lastHostToDisplayLatencyUs = 3;
    v2.lastBarcodeLatencyUs = 4;
    v2.protocolVersion = 2;
    const std::string j = toJson(v2);
    KV_CHECK_MSG(j == "{\"clientSendNs\":1,\"lastDecodedSeq\":2,\"lastHostToDisplayLatencyUs\":3,\"lastBarcodeLatencyUs\":4,"
                      "\"protocolVersion\":2}",
                 j);
    // A v1 decoder (an old host's: only the four v1 fields, read by name from the raw JSON) reads the
    // v2 ping exactly and doesn't trip on the new key, as JSONDecoder doesn't.
    {
        json::Value v;
        KV_REQUIRE(json::parse(j, v) && v.type == json::Value::Type::Object);
        uint64_t send_ns = 0, decoded = 0;
        uint32_t display_us = 0, barcode_us = 0;
        KV_CHECK(json::getU64(v, "clientSendNs", send_ns) && json::getU64(v, "lastDecodedSeq", decoded) &&
                 json::getU32(v, "lastHostToDisplayLatencyUs", display_us) &&
                 json::getU32(v, "lastBarcodeLatencyUs", barcode_us));
        KV_CHECK(send_ns == 1 && decoded == 2 && display_us == 3 && barcode_us == 4);
        KV_CHECK(v.members.size() == 5); // the four v1 fields, and protocolVersion beside them
    }
    // The v2 decoder reads the same JSON with the version.
    PingMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.clientSendNs == 1 && back.lastDecodedSeq == 2 && back.lastHostToDisplayLatencyUs == 3 &&
             back.lastBarcodeLatencyUs == 4 && back.protocolVersion == 2);
    // A v1 phone's ping has no protocolVersion; null counts as absent; a wrong type fails, as in Swift.
    PingMessage v1;
    KV_REQUIRE(fromJson(std::string("{\"clientSendNs\":5,\"lastDecodedSeq\":0,\"lastHostToDisplayLatencyUs\":0,"
                                    "\"lastBarcodeLatencyUs\":0}"),
                        v1));
    KV_CHECK(!v1.protocolVersion);
    KV_CHECK(toJson(v1).find("protocolVersion") == std::string::npos);
    PingMessage nul;
    KV_REQUIRE(fromJson(std::string("{\"protocolVersion\":null,\"clientSendNs\":5,\"lastDecodedSeq\":0,"
                                    "\"lastHostToDisplayLatencyUs\":0,\"lastBarcodeLatencyUs\":0}"),
                        nul));
    KV_CHECK(!nul.protocolVersion);
    PingMessage bad;
    KV_CHECK(!fromJson(std::string("{\"clientSendNs\":5,\"lastDecodedSeq\":0,\"lastHostToDisplayLatencyUs\":0,"
                                   "\"lastBarcodeLatencyUs\":0,\"protocolVersion\":\"2\"}"),
                       bad));
}

KV_TEST(host_state_sync_json_matches_the_phones_decoder) {
    HostStateSyncMessage m;
    m.stateSeq = 7;
    m.powerState = SensorPowerState::Off;
    m.modeRaw = 2;
    const std::string off = toJson(m);
    KV_CHECK_MSG(off == "{\"protocolVersion\":2,\"stateSeq\":7,\"powerState\":\"off\",\"modeRaw\":2,\"sbsLayoutRaw\":0,"
                        "\"allowPhoneModeSwitch\":true,\"allowPhoneSensorPower\":true,\"cutoffEnabled\":false,"
                        "\"cutoffNearMm\":500,\"cutoffFarMm\":2000,\"isHostFrozen\":false,\"isRecording\":false,"
                        "\"canRecordCurrentMode\":true,\"recordingReminderNeeded\":true,\"recordingFrames\":0,"
                        "\"recordingElapsedSec\":0,\"isReplayingOnHost\":false}",
                 off);

    HostStateSyncMessage busy = m;
    busy.stateSeq = 18446744073709551615ULL;
    busy.powerState = SensorPowerState::InUseElsewhere;
    busy.inUseHolder = "agent: Protonect pid: 4242";
    busy.allowPhoneSensorPower = false;
    busy.sensorPowerDisabledReason = text::kSensorPowerDisabledOnHost;
    busy.cutoffNearMm = 512.5f;
    busy.isRecording = true;
    busy.canRecordCurrentMode = false;
    busy.recordingDisabledReason = text::kColourLiveOnly;
    busy.recordingFrames = 300;
    busy.recordingElapsedSec = 10.25;
    const std::string j = toJson(busy);
    KV_CHECK_MSG(j == "{\"protocolVersion\":2,\"stateSeq\":18446744073709551615,\"powerState\":\"in_use_elsewhere\","
                      "\"inUseHolder\":\"agent: Protonect pid: 4242\",\"modeRaw\":2,\"sbsLayoutRaw\":0,"
                      "\"allowPhoneModeSwitch\":true,\"allowPhoneSensorPower\":false,"
                      "\"sensorPowerDisabledReason\":\"Power control is turned off on the Mac\",\"cutoffEnabled\":false,"
                      "\"cutoffNearMm\":512.5,\"cutoffFarMm\":2000,\"isHostFrozen\":false,\"isRecording\":true,"
                      "\"canRecordCurrentMode\":false,"
                      "\"recordingDisabledReason\":\"Colour is live only. Switch to IR or Depth to record.\","
                      "\"recordingReminderNeeded\":true,\"recordingFrames\":300,\"recordingElapsedSec\":10.25,"
                      "\"isReplayingOnHost\":false}",
                 j);
    HostStateSyncMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.stateSeq == busy.stateSeq && back.powerState == SensorPowerState::InUseElsewhere &&
             back.inUseHolder == busy.inUseHolder && !back.errorMessage && !back.allowPhoneSensorPower &&
             back.sensorPowerDisabledReason == busy.sensorPowerDisabledReason && back.cutoffNearMm == 512.5f &&
             back.recordingFrames == 300 && back.recordingElapsedSec == 10.25 && back.isRecording);

    // Every power state's wire name, both ways.
    for (auto s : {SensorPowerState::Off, SensorPowerState::Starting, SensorPowerState::On, SensorPowerState::InUseElsewhere,
                   SensorPowerState::Error}) {
        SensorPowerState r = SensorPowerState::Error;
        KV_CHECK(fromWire(toWire(s), r) && r == s);
    }
    KV_CHECK(std::string(toWire(SensorPowerState::InUseElsewhere)) == "in_use_elsewhere");

    // The phone's decoder: allowPhoneSensorPower may be missing (true) and optionals may be null.
    const std::string older = "{\"protocolVersion\":2,\"stateSeq\":1,\"powerState\":\"error\",\"errorMessage\":\"USB gone\","
                              "\"inUseHolder\":null,\"modeRaw\":0,\"sbsLayoutRaw\":0,\"allowPhoneModeSwitch\":false,"
                              "\"cutoffEnabled\":true,\"cutoffNearMm\":500.0,\"cutoffFarMm\":2000,\"isHostFrozen\":true,"
                              "\"isRecording\":false,\"canRecordCurrentMode\":true,\"recordingReminderNeeded\":false,"
                              "\"recordingFrames\":0,\"recordingElapsedSec\":0.0,\"isReplayingOnHost\":false}";
    HostStateSyncMessage o;
    KV_REQUIRE(fromJson(older, o));
    KV_CHECK(o.allowPhoneSensorPower && o.powerState == SensorPowerState::Error && o.errorMessage == std::string("USB gone") &&
             !o.inUseHolder && o.cutoffEnabled && o.isHostFrozen);
    // An unknown power state, or a bool sent as 0/1, fails the decode as it does on the phone.
    HostStateSyncMessage x;
    std::string bad_state = off;
    bad_state.replace(bad_state.find("\"off\""), 5, "\"sleep\"");
    KV_CHECK(!fromJson(bad_state, x));
    std::string bad_bool = off;
    bad_bool.replace(bad_bool.find("\"isRecording\":false"), 19, "\"isRecording\":0");
    KV_CHECK(!fromJson(bad_bool, x));
}

KV_TEST(phone_to_host_v2_messages_decode_as_the_phone_encodes_them) {
    // RemoteControlCommandMessage: nil optionals left out.
    RemoteControlCommandMessage sel;
    sel.action = RemoteControlAction::SelectMode;
    sel.modeRaw = 4;
    sel.sbsLayoutRaw = 0;
    KV_CHECK(toJson(sel) == "{\"action\":\"select_mode\",\"modeRaw\":4,\"sbsLayoutRaw\":0}");
    RemoteControlCommandMessage key;
    KV_CHECK(toJson(key) == "{\"action\":\"request_keyframe\"}");
    RemoteControlCommandMessage rec;
    KV_REQUIRE(fromJson(std::string("{\"snoozeReminderOneHour\":true,\"action\":\"start_recording\",\"reminderConfirmedOnPhone\":false}"),
                        rec));
    KV_CHECK(rec.action == RemoteControlAction::StartRecording && rec.reminderConfirmedOnPhone == false &&
             rec.snoozeReminderOneHour == true && !rec.modeRaw && !rec.hostFrozen);
    const char* actions[] = {"select_mode", "set_cutoff", "set_host_frozen", "start_recording", "stop_recording", "request_keyframe"};
    for (const char* a : actions) {
        RemoteControlCommandMessage c;
        KV_CHECK(fromJson(std::string("{\"action\":\"") + a + "\"}", c) && std::string(toWire(c.action)) == a);
    }
    RemoteControlCommandMessage unknown;
    KV_CHECK(!fromJson(std::string("{\"action\":\"format_disk\"}"), unknown));

    Orbit3DGestureMessage orbit;
    orbit.deltaYawRadians = 0.25f;
    orbit.deltaPitchRadians = -0.5f;
    KV_CHECK(toJson(orbit) == "{\"deltaYawRadians\":0.25,\"deltaPitchRadians\":-0.5,\"zoomScale\":1,\"panRightMeters\":0,"
                              "\"panUpMeters\":0,\"resetView\":false}");
    Orbit3DGestureMessage ob;
    KV_REQUIRE(fromJson(std::string("{\"deltaYawRadians\":0.1,\"deltaPitchRadians\":0,\"zoomScale\":0.9,\"panRightMeters\":0,"
                                    "\"panUpMeters\":0.02,\"resetView\":true}"),
                        ob));
    KV_CHECK(ob.deltaYawRadians == 0.1f && ob.zoomScale == 0.9f && ob.panUpMeters == 0.02f && ob.resetView);
    // Swift's description of a Float: 0.1f is written "0.1", not 0.10000000149011612.
    Orbit3DGestureMessage tenth;
    tenth.deltaYawRadians = 0.1f;
    KV_CHECK(toJson(tenth).find("\"deltaYawRadians\":0.1,") != std::string::npos);

    SensorPowerControlMessage pw;
    pw.requestId = 3;
    pw.turnOn = true;
    KV_CHECK(toJson(pw) == "{\"requestId\":3,\"turnOn\":true}");
    SensorPowerControlMessage pb;
    KV_REQUIRE(fromJson(std::string("{\"turnOn\":false,\"requestId\":18446744073709551615}"), pb));
    KV_CHECK(!pb.turnOn && pb.requestId == 18446744073709551615ULL);
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"turnOn\":1}"), pb));
}

KV_TEST(shared_sentences_match_the_mac) {
    KV_CHECK(std::string(text::kRecordingReminder) ==
             "Only you in view. Not pointed at a window. Depth and IR only. Deleted after 7 days.");
    KV_CHECK(text::kReminderSnoozeSeconds == 3600.0);
    KV_CHECK(std::string(text::kColourLiveOnly) == "Colour is live only. Switch to IR or Depth to record.");
    KV_CHECK(std::string(text::kSensorPowerDisabledOnHost) == "Power control is turned off on the Mac");
    KV_CHECK(text::inUseBy("  agent: Protonect pid: 4242 \n") == "The Kinect is in use by agent: Protonect pid: 4242");
    KV_CHECK(text::inUseBy("   ") == "The Kinect is in use by another process");
}

// ---- protocol v3: the pet camera (docs/protocol-v3-pet-camera.md) ------------------------------
// The expected JSON follows the same JSONEncoder rules as above, with keys in the order of the
// spec's tables. Synthetic names and ids only.

namespace {
// The JSON with one top-level "key":value pair removed. Only for values without a ',' in them.
std::string withoutKey(const std::string& j, const char* key) {
    const std::string mid = std::string(",\"") + key + "\":";
    size_t at = j.find(mid);
    if (at != std::string::npos) {
        const size_t end = j.find_first_of(",}", at + mid.size());
        return j.substr(0, at) + j.substr(end);
    }
    const std::string first = std::string("{\"") + key + "\":";
    if (j.rfind(first, 0) == 0) {
        const size_t end = j.find(',', first.size());
        return "{" + j.substr(end + 1);
    }
    return j;
}
// The JSON with the text after "key": replaced, up to the next ',' or '}'.
std::string withValue(const std::string& j, const char* key, const std::string& value) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = j.find(k);
    if (at == std::string::npos) return j;
    const size_t start = at + k.size();
    const size_t end = j.find_first_of(",}", start);
    return j.substr(0, start) + value + j.substr(end);
}
} // namespace

KV_TEST(v3_message_types_are_known_only_to_v3_readers) {
    KV_CHECK(kProtocolV3 == 3);
    KV_CHECK(kHostProtocolVersion == 3); // W6: the stream server speaks v3
    KV_CHECK(static_cast<uint8_t>(MsgType::PetCameraState) == 0x0B && static_cast<uint8_t>(MsgType::PetCameraCmd) == 0x0C &&
             static_cast<uint8_t>(MsgType::CommandResult) == 0x0D && static_cast<uint8_t>(MsgType::LiveViewReq) == 0x0E &&
             static_cast<uint8_t>(MsgType::SessionNotice) == 0x0F);
    for (int raw = 0; raw <= 0xFF; ++raw) {
        const uint8_t b = static_cast<uint8_t>(raw);
        const bool v1 = (raw >= 0x01 && raw <= 0x06) || raw == 0x10 || raw == 0x11;
        const bool v2 = v1 || (raw >= 0x07 && raw <= 0x0A);
        // W7 adds the camera roll's 0x12-0x19 (§1 rule 7); 0x1A and up stay free.
        const bool v3 = v2 || (raw >= 0x0B && raw <= 0x0F) || (raw >= 0x12 && raw <= 0x19);
        KV_CHECK_MSG(isKnownMsgType(b, 1) == v1, std::to_string(raw));
        KV_CHECK_MSG(isKnownMsgType(b, 2) == v2, std::to_string(raw));
        KV_CHECK_MSG(isKnownMsgType(b, 3) == v3, std::to_string(raw));
    }
    KV_CHECK(isKnownMsgType(0x12, 3) && isKnownMsgType(0x19, 3) && !isKnownMsgType(0x1A, 3) && !isKnownMsgType(0x7E, 3));
    // The default is the host's version, v3 since W6: it knows them all.
    KV_CHECK(isKnownMsgType(0x0B) && isKnownMsgType(0x0F) && isKnownMsgType(0x0A) && isKnownMsgType(0x12));

    MsgType t;
    uint8_t flags = 0;
    Bytes payload;
    const uint8_t v3_types[] = {0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
    for (const uint8_t ty : v3_types) {
        const uint8_t f[] = {'K', 'V', ty, 0, 0, 0, 0, 2, '{', '}'};
        // A v3 reader reads them.
        FrameReader v3(kProtocolV3);
        v3.append(f, sizeof(f));
        KV_CHECK(v3.next(t, flags, payload) == FrameReader::Result::Frame && static_cast<uint8_t>(t) == ty);
        // A v2 phone's stream reader skips them whole; a strict v1 reader would drop the connection,
        // which is why the host sends them only to a v3 connection.
        FrameReader v2(2, true);
        v2.append(f, sizeof(f));
        KV_CHECK(v2.next(t, flags, payload) == FrameReader::Result::Unknown && static_cast<uint8_t>(t) == ty);
        FrameReader v1(1);
        v1.append(f, sizeof(f));
        KV_CHECK(v1.next(t, flags, payload) == FrameReader::Result::Violation);
    }
}

KV_TEST(v3_hello_without_pet_camera_is_byte_identical) {
    HelloServerMessage m;
    m.protocolVersion = 1;
    m.modeRaw = 3;
    m.sbsLayoutRaw = 1;
    m.width = 1280;
    m.height = 424;
    KV_CHECK(!m.petCamera);
    KV_CHECK(toJson(m) == kGoldenHelloJson);
    HelloServerMessage back;
    back.petCamera = PetCameraHello{};
    KV_REQUIRE(fromJson(std::string(kGoldenHelloJson), back));
    KV_CHECK(!back.petCamera); // decoding clears an earlier value
    // null counts as absent.
    std::string with_null = kGoldenHelloJson;
    with_null.insert(with_null.size() - 1, ",\"petCamera\":null");
    KV_CHECK(fromJson(with_null, back) && !back.petCamera);
}

KV_TEST(v3_hello_with_pet_camera_and_old_readers_still_decode_it) {
    HelloServerMessage m;
    m.protocolVersion = kProtocolV3;
    m.modeRaw = 3;
    m.sbsLayoutRaw = 1;
    m.allowPhoneModeSwitch = false; // a viewer-role device
    m.width = 1280;
    m.height = 424;
    m.petCamera = PetCameraHello{}; // what a W6 host sends
    const std::string j = toJson(m);
    KV_CHECK_MSG(j == "{\"protocolVersion\":3,\"modeRaw\":3,\"sbsLayoutRaw\":1,\"allowPhoneModeSwitch\":false,\"width\":1280,"
                      "\"height\":424,\"codecRaw\":0,\"petCamera\":{\"specVersion\":1,\"maxWatchers\":4,\"cameraRoll\":false,"
                      "\"outside\":false,\"qualities\":[\"auto\",\"high\",\"medium\",\"low\"]}}",
                 j);
    KV_CHECK(toJson(*m.petCamera) == "{\"specVersion\":1,\"maxWatchers\":4,\"cameraRoll\":false,\"outside\":false,"
                                     "\"qualities\":[\"auto\",\"high\",\"medium\",\"low\"]}");

    // v1 and v2 phones read 0x01 as before: the frame is known to both readers, the hello decodes,
    // and an old decoder that reads only the seven v1 keys by name doesn't trip on the extra one.
    const Bytes frame = packFrame(MsgType::HelloServer, 0, j);
    for (int version : {1, 2}) {
        FrameReader r(version);
        r.append(frame.data(), frame.size());
        MsgType t;
        uint8_t flags = 0;
        Bytes payload;
        KV_REQUIRE(r.next(t, flags, payload) == FrameReader::Result::Frame);
        KV_CHECK(t == MsgType::HelloServer);
        HelloServerMessage h;
        KV_REQUIRE(fromJson(payload, h));
        KV_CHECK(h.protocolVersion == 3 && h.modeRaw == 3 && h.sbsLayoutRaw == 1 && !h.allowPhoneModeSwitch &&
                 h.width == 1280 && h.height == 424 && h.codecRaw == 0);
        json::Value v;
        KV_REQUIRE(json::parse(S(payload), v));
        int pv = 0, mode = 0, sbs = 0, w = 0, ht = 0;
        bool allow = true;
        uint8_t codec = 9;
        KV_CHECK(json::getInt(v, "protocolVersion", pv) && json::getInt(v, "modeRaw", mode) &&
                 json::getInt(v, "sbsLayoutRaw", sbs) && json::getBool(v, "allowPhoneModeSwitch", allow) &&
                 json::getInt(v, "width", w) && json::getInt(v, "height", ht) && json::getU8(v, "codecRaw", codec));
        KV_CHECK(pv == 3 && mode == 3 && !allow && w == 1280 && codec == 0);
        KV_CHECK(v.members.size() == 8);
    }

    // The v3 decoder reads the offer.
    HelloServerMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_REQUIRE(back.petCamera.has_value());
    KV_CHECK(back.petCamera->specVersion == 1 && back.petCamera->maxWatchers == 4 && !back.petCamera->cameraRoll &&
             !back.petCamera->outside &&
             back.petCamera->qualities == (std::vector<std::string>{"auto", "high", "medium", "low"}));
    // A quality this build doesn't know is kept as a string (lenient).
    PetCameraHello h;
    KV_CHECK(fromJson(std::string("{\"specVersion\":2,\"maxWatchers\":6,\"cameraRoll\":true,\"outside\":true,"
                                  "\"qualities\":[\"auto\",\"ultra\"]}"),
                      h) &&
             h.qualities.size() == 2 && liveQualityFromWire(h.qualities[1]) == LiveQuality::Unknown);
    // A petCamera present but malformed fails the hello, as Swift's decodeIfPresent does.
    std::string bad = j;
    bad.replace(bad.find("\"maxWatchers\":4"), 15, "\"maxWatchers\":\"4\"");
    KV_CHECK(!fromJson(bad, back));
    const std::string qualities = ",\"qualities\":[\"auto\",\"high\",\"medium\",\"low\"]";
    std::string no_qualities = j;
    no_qualities.replace(no_qualities.find(qualities), qualities.size(), "");
    json::Value parsed;
    KV_CHECK(json::parse(no_qualities, parsed));
    KV_CHECK(!fromJson(no_qualities, back)); // "qualities" is required in petCamera
    std::string mixed = j;
    mixed.replace(mixed.find("\"low\""), 5, "7");
    KV_CHECK(!fromJson(mixed, back));
    std::string not_object = kGoldenHelloJson;
    not_object.insert(not_object.size() - 1, ",\"petCamera\":true");
    KV_CHECK(!fromJson(not_object, back));
    // Each required key of PetCameraHello.
    const std::string offer = toJson(PetCameraHello{});
    for (const char* k : {"specVersion", "maxWatchers", "cameraRoll", "outside"}) {
        const std::string cut = withoutKey(offer, k);
        PetCameraHello x;
        KV_CHECK_MSG(cut.size() < offer.size() && json::parse(cut, parsed), k);
        KV_CHECK_MSG(!fromJson(cut, x), k);
    }
}

KV_TEST(v3_pair_request_and_response_optional_keys) {
    KV_CHECK(static_cast<uint8_t>(PairingMethod::Invite) == 2);

    // A v3 device's request: the new keys after the v1 ones; an old phone's is unchanged.
    PairRequestMessage req;
    req.methodRaw = static_cast<uint8_t>(PairingMethod::Invite);
    req.clientCertDER = Bytes{0x30, 0x03, 0x02, 0x01, 0x00};
    req.clientProofHMAC = Bytes{0xFF, 0xFF};
    const std::string old = toJson(req);
    KV_CHECK_MSG(old == "{\"methodRaw\":2,\"clientCertDER\":\"MAMCAQA=\",\"clientProofHMAC\":\"\\/\\/8=\"}", old);
    req.protocolVersion = 3;
    req.deviceName = "Kitchen iPad";
    req.deviceKind = "ipad";
    const std::string j = toJson(req);
    KV_CHECK_MSG(j == "{\"methodRaw\":2,\"clientCertDER\":\"MAMCAQA=\",\"clientProofHMAC\":\"\\/\\/8=\",\"protocolVersion\":3,"
                      "\"deviceName\":\"Kitchen iPad\",\"deviceKind\":\"ipad\"}",
                 j);
    PairRequestMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.methodRaw == 2 && back.protocolVersion == 3 && back.deviceName == std::string("Kitchen iPad") &&
             back.deviceKind == std::string("ipad") && back.clientProofHMAC == req.clientProofHMAC);
    PairRequestMessage plain;
    plain.deviceName = "stale";
    KV_REQUIRE(fromJson(old, plain));
    KV_CHECK(!plain.protocolVersion && !plain.deviceName && !plain.deviceKind);
    // An unknown kind still decodes; the host maps it to "other".
    PairRequestMessage odd;
    KV_REQUIRE(fromJson(withValue(j, "deviceKind", "\"toaster\""), odd));
    KV_CHECK(odd.deviceKind == std::string("toaster") && normalizeDeviceKind(odd.deviceKind) == "other");
    KV_CHECK(!fromJson(withValue(j, "protocolVersion", "\"3\""), odd));
    KV_CHECK(!fromJson(withValue(j, "deviceName", "5"), odd));
    for (const char* k : {"iphone", "ipad", "mac", "other"}) KV_CHECK(normalizeDeviceKind(std::string(k)) == k);
    KV_CHECK(normalizeDeviceKind(std::nullopt) == "other");
    KV_CHECK(normalizeDeviceKind(std::string("iPhone")) == "other");

    // The response: unchanged without the keys, the keys after "message" with them.
    PairResponseMessage ok;
    ok.accepted = true;
    ok.serviceId = "a1b2c3d4e5f6";
    ok.streamPort = 50123;
    ok.serverProofHMAC = B(kGoldenProof32, kGoldenProof32_len);
    ok.message = "Paired";
    KV_CHECK(toJson(ok) == kGoldenPairResponseJson);
    ok.role = toWire(PetRole::Owner);
    ok.deviceId = "0123456789abcdef";
    ok.deviceName = "iPhone";
    ok.protocolVersion = 3;
    std::string expected = kGoldenPairResponseJson;
    expected.insert(expected.size() - 1,
                    ",\"role\":\"owner\",\"deviceId\":\"0123456789abcdef\",\"deviceName\":\"iPhone\",\"protocolVersion\":3");
    const std::string rj = toJson(ok);
    KV_CHECK_MSG(rj == expected, rj);
    PairResponseMessage rb;
    KV_REQUIRE(fromJson(rj, rb));
    KV_CHECK(rb.accepted && rb.role == std::string("owner") && rb.deviceId == std::string("0123456789abcdef") &&
             rb.deviceName == std::string("iPhone") && rb.protocolVersion == 3);
    PairResponseMessage reject;
    reject.role = "stale";
    KV_REQUIRE(fromJson(std::string(kGoldenPairRejectJson), reject));
    KV_CHECK(!reject.role && !reject.deviceId && !reject.deviceName && !reject.protocolVersion);
    // A frame this size still fits the phone's single 16 KiB receive with a wide margin.
    KV_CHECK(rj.size() < 1024);
}

KV_TEST(v3_enums_round_trip_and_decode_leniently) {
    for (auto v : {PetRole::Owner, PetRole::Viewer}) KV_CHECK(petRoleFromWire(toWire(v)) == v);
    for (auto v : {PetPath::Home, PetPath::Outside}) KV_CHECK(petPathFromWire(toWire(v)) == v);
    for (auto v : {LiveTier::High, LiveTier::Medium, LiveTier::Low}) KV_CHECK(liveTierFromWire(toWire(v)) == v);
    for (auto v : {LiveQuality::Auto, LiveQuality::High, LiveQuality::Medium, LiveQuality::Low})
        KV_CHECK(liveQualityFromWire(toWire(v)) == v);
    for (auto v : {ClipTrigger::Manual, ClipTrigger::Motion, ClipTrigger::Schedule})
        KV_CHECK(clipTriggerFromWire(toWire(v)) == v);
    KV_CHECK(std::string(toWire(PetRole::Owner)) == "owner" && std::string(toWire(PetRole::Viewer)) == "viewer");
    KV_CHECK(std::string(toWire(PetPath::Home)) == "home" && std::string(toWire(PetPath::Outside)) == "outside");
    KV_CHECK(std::string(toWire(LiveQuality::Auto)) == "auto" && std::string(toWire(LiveTier::Medium)) == "medium");
    KV_CHECK(std::string(toWire(ClipTrigger::Schedule)) == "schedule");
    // Unknown values become Unknown, never a failure; matching is exact (snake_case, lower case).
    KV_CHECK(petRoleFromWire("admin") == PetRole::Unknown && petRoleFromWire("Owner") == PetRole::Unknown &&
             petRoleFromWire("") == PetRole::Unknown && petRoleFromWire("unknown") == PetRole::Unknown);
    KV_CHECK(petPathFromWire("satellite") == PetPath::Unknown);
    KV_CHECK(liveTierFromWire("auto") == LiveTier::Unknown); // auto is a quality, not a tier
    KV_CHECK(liveQualityFromWire("ultra") == LiveQuality::Unknown);
    KV_CHECK(clipTriggerFromWire("sound") == ClipTrigger::Unknown);
    KV_CHECK(std::string(toWire(PetRole::Unknown)) == "unknown" && std::string(toWire(PetPath::Unknown)) == "unknown" &&
             std::string(toWire(LiveTier::Unknown)) == "unknown" && std::string(toWire(LiveQuality::Unknown)) == "unknown" &&
             std::string(toWire(ClipTrigger::Unknown)) == "unknown");

    // Every action and error code, as the spec spells them.
    const char* actions[] = {"set_sharing", "set_privacy", "set_roll", "set_motion", "start_clip",
                             "stop_clip", "star_clip", "delete_clip", "revoke_device"};
    for (const char* a : actions) KV_CHECK_MSG(pet_action::isKnown(a), a);
    KV_CHECK(!pet_action::isKnown("format_disk") && !pet_action::isKnown("select_mode"));
    const char* errors[] = {"not_allowed_for_role", "privacy_mode", "sharing_off", "sharing_schedule", "roll_disabled",
                            "roll_preparing", "folder_unavailable", "sensor_off", "view_frozen", "pc_replaying", "disk_floor", "storage_full",
                            "too_many_viewers", "not_found", "busy", "invalid", "unsupported", "internal"};
    for (const char* e : errors) KV_CHECK_MSG(pet_error::isKnown(e), e);
    KV_CHECK(!pet_error::isKnown("ok") && !pet_error::isKnown("revoked"));
}

namespace {
// A viewer's 0x0B from a W6 host: no optional key at all.
PetCameraStateMessage viewerState() {
    PetCameraStateMessage m;
    m.stateSeq = 42;
    m.deviceId = "0123456789abcdef";
    m.deviceName = "Kitchen iPad";
    m.role = PetRole::Viewer;
    m.path = PetPath::Home;
    m.sharingEnabled = true;
    m.liveAllowed = true;
    m.watching = true;
    m.liveQuality = toWire(LiveQuality::Auto);
    m.liveTier = toWire(LiveTier::Medium);
    return m;
}
const char* const kViewerStateJson =
    "{\"protocolVersion\":3,\"stateSeq\":42,\"deviceId\":\"0123456789abcdef\",\"deviceName\":\"Kitchen iPad\","
    "\"role\":\"viewer\",\"path\":\"home\",\"sharingEnabled\":true,\"sharingScheduleOff\":false,\"outsideEnabled\":false,"
    "\"privacyMode\":false,\"liveAllowed\":true,\"watching\":true,\"liveQuality\":\"auto\",\"liveTier\":\"medium\","
    "\"rollAvailable\":false,\"playbackAvailable\":false,\"rollEnabled\":false,\"motionEnabled\":false,"
    "\"motionAvailable\":false,\"motionSensitivity\":5,\"scheduleEnabled\":false,\"clipRecording\":false,"
    "\"clipManualActive\":false,\"clipElapsedSec\":0}";
} // namespace

KV_TEST(v3_pet_camera_state_viewer_json_and_required_keys) {
    const std::string j = toJson(viewerState());
    KV_CHECK_MSG(j == kViewerStateJson, j);
    // The W6 defaults (§5.1 "Before W7 and W8").
    PetCameraStateMessage d;
    KV_CHECK(d.protocolVersion == 3 && !d.rollAvailable && !d.playbackAvailable && !d.rollEnabled && !d.motionEnabled &&
             !d.motionAvailable && !d.scheduleEnabled && !d.clipRecording && !d.clipManualActive &&
             d.motionSensitivity == 5 && d.clipElapsedSec == 0.0 && !d.outsideEnabled && d.liveQuality == "auto" &&
             d.liveTier == "high" && !d.rollBlockedReason && !d.outsideEndpoint && !d.outsideStatus && !d.watchingNow &&
             !d.devices);

    PetCameraStateMessage back;
    back.watchingNow = std::vector<PetWatcher>{PetWatcher{}};
    back.outsideEndpoint = PetEndpoint{};
    back.scheduleSummary = "stale";
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.stateSeq == 42 && back.deviceId == "0123456789abcdef" && back.deviceName == "Kitchen iPad" &&
             back.role == PetRole::Viewer && back.path == PetPath::Home && back.sharingEnabled && back.liveAllowed &&
             back.watching && back.liveQuality == "auto" && back.liveTier == "medium" && back.motionSensitivity == 5);
    // Owner-only keys are absent for a viewer, and decoding clears what was there before.
    KV_CHECK(!back.watchingNow && !back.devices && !back.outsideStatus && !back.outsideStatusText &&
             !back.outsideEndpoint && !back.scheduleSummary && !back.liveUnavailableReason);
    KV_CHECK(j.find("watchingNow") == std::string::npos && j.find("devices") == std::string::npos);

    // Every required key, one at a time: the message fails and is ignored (§5.10).
    const char* required[] = {"protocolVersion", "stateSeq", "deviceId", "deviceName", "role", "path", "sharingEnabled",
                              "sharingScheduleOff", "outsideEnabled", "privacyMode", "liveAllowed", "watching",
                              "liveQuality", "liveTier", "rollAvailable", "playbackAvailable", "rollEnabled",
                              "motionEnabled", "motionAvailable", "motionSensitivity", "scheduleEnabled",
                              "clipRecording", "clipManualActive", "clipElapsedSec"};
    json::Value parsed;
    KV_REQUIRE(json::parse(j, parsed));
    KV_CHECK(parsed.members.size() == sizeof(required) / sizeof(required[0])); // the list is complete
    for (const char* k : required) {
        const std::string cut = withoutKey(j, k);
        json::Value still_json;
        KV_CHECK_MSG(cut.size() < j.size() && json::parse(cut, still_json), k);
        PetCameraStateMessage x;
        KV_CHECK_MSG(!fromJson(cut, x), k);
    }
    // Wrong types fail too: bools as 0/1, numbers as strings, a negative u64, a non-string enum.
    PetCameraStateMessage x;
    KV_CHECK(!fromJson(withValue(j, "sharingEnabled", "1"), x));
    KV_CHECK(!fromJson(withValue(j, "stateSeq", "-1"), x));
    KV_CHECK(!fromJson(withValue(j, "stateSeq", "\"42\""), x));
    KV_CHECK(!fromJson(withValue(j, "role", "1"), x));
    KV_CHECK(!fromJson(withValue(j, "motionSensitivity", "\"5\""), x));
    KV_CHECK(!fromJson(withValue(j, "motionSensitivity", "5.5"), x));
    KV_CHECK(!fromJson(withValue(j, "clipElapsedSec", "\"0\""), x));
    KV_CHECK(!fromJson(withValue(j, "liveQuality", "null"), x));
    KV_CHECK(!fromJson(std::string("[]"), x));
    KV_CHECK(!fromJson(std::string("{\"protocolVersion\":3"), x));
    // A whole-number Double may arrive as 0.0 or 3.25 from another encoder.
    KV_CHECK(fromJson(withValue(j, "clipElapsedSec", "3.25"), x) && x.clipElapsedSec == 3.25);
}

KV_TEST(v3_pet_camera_state_owner_json_with_every_key) {
    PetCameraStateMessage m;
    m.stateSeq = 18446744073709551615ULL;
    m.deviceId = "fedcba9876543210";
    m.deviceName = "Owner iPhone";
    m.role = PetRole::Owner;
    m.path = PetPath::Outside;
    m.sharingEnabled = true;
    m.sharingScheduleOff = true;
    m.outsideEnabled = true;
    m.privacyMode = false;
    m.liveAllowed = false;
    m.liveUnavailableReason = live_unavailable::kTooManyViewers;
    m.watching = false;
    m.liveQuality = toWire(LiveQuality::Low);
    m.liveTier = toWire(LiveTier::High);
    m.rollAvailable = true;
    m.playbackAvailable = false;
    m.rollEnabled = true;
    m.motionEnabled = true;
    m.motionAvailable = true;
    m.motionSensitivity = 7;
    m.scheduleEnabled = true;
    m.scheduleSummary = "Every day 08:00\xE2\x80\x93" "08:10"; // en dash, sent as raw UTF-8
    m.clipRecording = true;
    m.clipTrigger = toWire(ClipTrigger::Motion);
    m.clipManualActive = true;
    m.clipElapsedSec = 12.5;
    m.rollBlockedReason = roll_blocked::kDiskFloor;
    m.rollBlockedText = text::kDiskFloor;
    m.outsideEndpoint = PetEndpoint{"203.0.113.7", 50443, true};
    m.outsideCandidates = std::vector<PetEndpoint>{PetEndpoint{"203.0.113.7", 50443, true}, PetEndpoint{"100.100.1.2", 50443, false}};
    m.outsideStatus = outside_status::kMapped;
    m.outsideStatusText = "Router mapped port 50443/TCP";
    m.watchingNow = std::vector<PetWatcher>{
        PetWatcher{"fedcba9876543210", "Owner iPhone", PetRole::Owner, PetPath::Outside, watcher_activity::kLive},
        PetWatcher{"0123456789abcdef", "Kitchen iPad", PetRole::Viewer, PetPath::Home, watcher_activity::kPlayback},
    };
    m.devices = std::vector<PetDevice>{
        PetDevice{"fedcba9876543210", "Owner iPhone", PetRole::Owner, device_kind::kIphone, 1763452800000ULL,
                  1763456400000ULL, true},
        PetDevice{"0123456789abcdef", "Kitchen iPad", PetRole::Viewer, device_kind::kIpad, 1763452900000ULL, std::nullopt,
                  false},
    };
    const std::string j = toJson(m);
    const std::string expected =
        "{\"protocolVersion\":3,\"stateSeq\":18446744073709551615,\"deviceId\":\"fedcba9876543210\","
        "\"deviceName\":\"Owner iPhone\",\"role\":\"owner\",\"path\":\"outside\",\"sharingEnabled\":true,"
        "\"sharingScheduleOff\":true,\"outsideEnabled\":true,\"privacyMode\":false,\"liveAllowed\":false,"
        "\"liveUnavailableReason\":\"too_many_viewers\",\"watching\":false,\"liveQuality\":\"low\",\"liveTier\":\"high\","
        "\"rollAvailable\":true,\"playbackAvailable\":false,\"rollEnabled\":true,\"motionEnabled\":true,"
        "\"motionAvailable\":true,\"motionSensitivity\":7,\"scheduleEnabled\":true,"
        "\"scheduleSummary\":\"Every day 08:00\xE2\x80\x93" "08:10\",\"clipRecording\":true,\"clipTrigger\":\"motion\","
        "\"clipManualActive\":true,\"clipElapsedSec\":12.5,\"rollBlockedReason\":\"disk_floor\","
        "\"rollBlockedText\":\"Recording stopped: less than 20 GB is free on the PC's disk.\","
        "\"outsideEndpoint\":{\"host\":\"203.0.113.7\",\"port\":50443,\"verified\":true},"
        "\"outsideCandidates\":[{\"host\":\"203.0.113.7\",\"port\":50443,\"verified\":true},"
        "{\"host\":\"100.100.1.2\",\"port\":50443,\"verified\":false}],\"outsideStatus\":\"mapped\","
        "\"outsideStatusText\":\"Router mapped port 50443\\/TCP\","
        "\"watchingNow\":[{\"deviceId\":\"fedcba9876543210\",\"deviceName\":\"Owner iPhone\",\"role\":\"owner\","
        "\"path\":\"outside\",\"activity\":\"live\"},{\"deviceId\":\"0123456789abcdef\",\"deviceName\":\"Kitchen iPad\","
        "\"role\":\"viewer\",\"path\":\"home\",\"activity\":\"playback\"}],"
        "\"devices\":[{\"deviceId\":\"fedcba9876543210\",\"name\":\"Owner iPhone\",\"role\":\"owner\",\"kind\":\"iphone\","
        "\"pairedAtUnixMs\":1763452800000,\"lastSeenUnixMs\":1763456400000,\"connected\":true},"
        "{\"deviceId\":\"0123456789abcdef\",\"name\":\"Kitchen iPad\",\"role\":\"viewer\",\"kind\":\"ipad\","
        "\"pairedAtUnixMs\":1763452900000,\"connected\":false}]}";
    KV_CHECK_MSG(j == expected, j);

    PetCameraStateMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.stateSeq == m.stateSeq && back.role == PetRole::Owner && back.path == PetPath::Outside &&
             back.sharingScheduleOff && back.outsideEnabled && back.liveUnavailableReason == m.liveUnavailableReason &&
             back.liveQuality == "low" && back.motionSensitivity == 7 && back.scheduleSummary == m.scheduleSummary &&
             back.clipTrigger == std::string("motion") && back.clipElapsedSec == 12.5 &&
             back.rollBlockedReason == std::string("disk_floor") && back.rollBlockedText == m.rollBlockedText &&
             back.outsideStatus == std::string("mapped") && back.outsideStatusText == m.outsideStatusText);
    KV_REQUIRE(back.outsideEndpoint.has_value());
    KV_CHECK(back.outsideEndpoint->host == "203.0.113.7" && back.outsideEndpoint->port == 50443 &&
             back.outsideEndpoint->verified);
    KV_REQUIRE(back.watchingNow && back.watchingNow->size() == 2);
    KV_CHECK((*back.watchingNow)[0].deviceId == "fedcba9876543210" && (*back.watchingNow)[0].role == PetRole::Owner &&
             (*back.watchingNow)[0].path == PetPath::Outside && (*back.watchingNow)[0].activity == "live" &&
             (*back.watchingNow)[1].role == PetRole::Viewer && (*back.watchingNow)[1].activity == "playback");
    KV_REQUIRE(back.devices && back.devices->size() == 2);
    KV_CHECK((*back.devices)[0].kind == "iphone" && (*back.devices)[0].pairedAtUnixMs == 1763452800000ULL &&
             (*back.devices)[0].lastSeenUnixMs == 1763456400000ULL && (*back.devices)[0].connected);
    KV_CHECK((*back.devices)[1].name == "Kitchen iPad" && !(*back.devices)[1].lastSeenUnixMs && !(*back.devices)[1].connected);
    KV_CHECK(toJson(back) == j); // a full round trip

    // Set but empty lists are written as [], not left out (left out means "not for this role").
    PetCameraStateMessage empty = viewerState();
    empty.role = PetRole::Owner;
    empty.watchingNow = std::vector<PetWatcher>{};
    empty.devices = std::vector<PetDevice>{};
    const std::string ej = toJson(empty);
    KV_CHECK(ej.find(",\"watchingNow\":[],\"devices\":[]}") != std::string::npos);
    PetCameraStateMessage eb;
    KV_CHECK(fromJson(ej, eb) && eb.watchingNow && eb.watchingNow->empty() && eb.devices && eb.devices->empty());

    // Lenient enums inside the message and its nested objects: a newer host's values still decode.
    std::string newer = j;
    newer.replace(newer.find("\"role\":\"owner\""), 14, "\"role\":\"guardian\"");
    newer.replace(newer.find("\"path\":\"outside\""), 16, "\"path\":\"relay\"");
    newer.replace(newer.find("\"liveQuality\":\"low\""), 19, "\"liveQuality\":\"ultra\"");
    newer.replace(newer.find("\"clipTrigger\":\"motion\""), 22, "\"clipTrigger\":\"sound\"");
    newer.replace(newer.find("\"role\":\"viewer\",\"path\":\"home\""), 29, "\"role\":\"guest\",\"path\":\"moon\"");
    newer.replace(newer.find("\"kind\":\"ipad\""), 13, "\"kind\":\"watch\"");
    newer.insert(newer.size() - 1, ",\"futureKey\":{\"a\":[1,2]}");
    PetCameraStateMessage nb;
    KV_REQUIRE(fromJson(newer, nb));
    KV_CHECK(nb.role == PetRole::Unknown && nb.path == PetPath::Unknown && nb.liveQuality == "ultra" &&
             liveQualityFromWire(nb.liveQuality) == LiveQuality::Unknown &&
             clipTriggerFromWire(*nb.clipTrigger) == ClipTrigger::Unknown);
    KV_CHECK((*nb.watchingNow)[1].role == PetRole::Unknown && (*nb.watchingNow)[1].path == PetPath::Unknown &&
             (*nb.devices)[1].kind == "watch");

    // A malformed nested object fails the whole message.
    PetCameraStateMessage x;
    std::string bad_port = j;
    bad_port.replace(bad_port.find("\"port\":50443"), 12, "\"port\":70000");
    KV_CHECK(!fromJson(bad_port, x));
    std::string no_activity = j;
    no_activity.replace(no_activity.find(",\"activity\":\"live\""), 18, "");
    KV_CHECK(!fromJson(no_activity, x));
    std::string no_paired = j;
    no_paired.replace(no_paired.find("\"pairedAtUnixMs\":1763452900000,"), 31, "");
    KV_CHECK(!fromJson(no_paired, x));
    std::string seen_string = j;
    seen_string.replace(seen_string.find("\"lastSeenUnixMs\":1763456400000"), 30, "\"lastSeenUnixMs\":\"1\"");
    KV_CHECK(!fromJson(seen_string, x));
    std::string list_not_array = kViewerStateJson;
    list_not_array.insert(list_not_array.size() - 1, ",\"devices\":{}");
    KV_CHECK(!fromJson(list_not_array, x));
    std::string list_of_strings = kViewerStateJson;
    list_of_strings.insert(list_of_strings.size() - 1, ",\"watchingNow\":[\"a\"]");
    KV_CHECK(!fromJson(list_of_strings, x));
    std::string endpoint_not_object = kViewerStateJson;
    endpoint_not_object.insert(endpoint_not_object.size() - 1, ",\"outsideEndpoint\":\"203.0.113.7\"");
    KV_CHECK(!fromJson(endpoint_not_object, x));
    // null optionals are absent.
    std::string nulls = kViewerStateJson;
    nulls.insert(nulls.size() - 1, ",\"outsideEndpoint\":null,\"devices\":null,\"clipTrigger\":null");
    KV_CHECK(fromJson(nulls, x) && !x.outsideEndpoint && !x.devices && !x.clipTrigger);

    // The nested objects on their own.
    PetEndpoint e;
    KV_CHECK(toJson(PetEndpoint{"pc.example", 1, false}) == "{\"host\":\"pc.example\",\"port\":1,\"verified\":false}");
    KV_CHECK(fromJson(std::string("{\"verified\":false,\"port\":65535,\"host\":\"h\"}"), e) && e.port == 65535);
    KV_CHECK(!fromJson(std::string("{\"host\":\"h\",\"port\":1}"), e));
    PetWatcher w;
    KV_CHECK(!fromJson(std::string("{\"deviceId\":\"a\",\"deviceName\":\"b\",\"role\":\"owner\",\"activity\":\"live\"}"), w));
    PetDevice dv;
    KV_CHECK(fromJson(std::string("{\"deviceId\":\"a\",\"name\":\"b\",\"role\":\"viewer\",\"kind\":\"mac\","
                                  "\"pairedAtUnixMs\":1,\"lastSeenUnixMs\":null,\"connected\":true}"),
                      dv) &&
             !dv.lastSeenUnixMs && dv.kind == "mac");
    KV_CHECK(!fromJson(std::string("{\"deviceId\":\"a\",\"name\":\"b\",\"role\":\"viewer\",\"kind\":\"mac\","
                                   "\"pairedAtUnixMs\":1}"),
                       dv));
}

KV_TEST(v3_pet_camera_cmd_json) {
    PetCameraCommandMessage motion;
    motion.requestId = 7;
    motion.action = pet_action::kSetMotion;
    motion.enabled = true;
    motion.sensitivity = 10;
    KV_CHECK(toJson(motion) == "{\"requestId\":7,\"action\":\"set_motion\",\"enabled\":true,\"sensitivity\":10}");
    PetCameraCommandMessage star;
    star.requestId = 8;
    star.action = pet_action::kStarClip;
    star.clipId = "clip/0001";
    star.starred = false;
    KV_CHECK(toJson(star) == "{\"requestId\":8,\"action\":\"star_clip\",\"clipId\":\"clip\\/0001\",\"starred\":false}");
    PetCameraCommandMessage revoke;
    revoke.requestId = 18446744073709551615ULL;
    revoke.action = pet_action::kRevokeDevice;
    revoke.deviceId = "0123456789abcdef";
    KV_CHECK(toJson(revoke) ==
             "{\"requestId\":18446744073709551615,\"action\":\"revoke_device\",\"deviceId\":\"0123456789abcdef\"}");
    PetCameraCommandMessage all;
    all.requestId = 1;
    all.action = "x";
    all.enabled = false;
    all.sensitivity = 1;
    all.clipId = "c";
    all.starred = true;
    all.deviceId = "d";
    const std::string aj = toJson(all);
    KV_CHECK_MSG(aj == "{\"requestId\":1,\"action\":\"x\",\"enabled\":false,\"sensitivity\":1,\"clipId\":\"c\",\"starred\":true,"
                       "\"deviceId\":\"d\"}",
                 aj);
    PetCameraCommandMessage back;
    KV_REQUIRE(fromJson(aj, back));
    KV_CHECK(back.requestId == 1 && back.action == "x" && back.enabled == false && back.sensitivity == 1 &&
             back.clipId == std::string("c") && back.starred == true && back.deviceId == std::string("d"));
    KV_REQUIRE(fromJson(toJson(revoke), back));
    KV_CHECK(back.requestId == revoke.requestId && back.action == "revoke_device" && !back.enabled && !back.sensitivity &&
             !back.clipId && !back.starred && back.deviceId == std::string("0123456789abcdef"));

    // An unknown action still decodes, so the host can answer "unsupported" (§1 rule 6, §5.10).
    PetCameraCommandMessage unknown;
    KV_REQUIRE(fromJson(std::string("{\"action\":\"format_disk\",\"requestId\":3}"), unknown));
    KV_CHECK(unknown.action == "format_disk" && unknown.requestId == 3 && !pet_action::isKnown(unknown.action));
    // Out-of-range sensitivity decodes too; the host answers "invalid".
    KV_CHECK(fromJson(std::string("{\"requestId\":4,\"action\":\"set_motion\",\"sensitivity\":99}"), unknown) &&
             unknown.sensitivity == 99);
    // Malformed: not answered.
    PetCameraCommandMessage x;
    KV_CHECK(!fromJson(std::string("{\"action\":\"set_roll\",\"enabled\":true}"), x)); // no requestId
    KV_CHECK(!fromJson(std::string("{\"requestId\":5}"), x));                          // no action
    KV_CHECK(!fromJson(std::string("{\"requestId\":\"5\",\"action\":\"set_roll\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":-5,\"action\":\"set_roll\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":5,\"action\":7}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":5,\"action\":\"set_roll\",\"enabled\":1}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":5,\"action\":\"set_motion\",\"sensitivity\":\"5\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":5,\"action\":\"star_clip\",\"clipId\":5}"), x));
    KV_CHECK(fromJson(std::string("{\"requestId\":5,\"action\":\"set_roll\",\"enabled\":null}"), x) && !x.enabled);
}

KV_TEST(v3_command_result_json) {
    CommandResultMessage ok;
    ok.requestId = 7;
    ok.ok = true;
    KV_CHECK(toJson(ok) == "{\"requestId\":7,\"ok\":true}");
    CommandResultMessage err;
    err.requestId = 8;
    err.error = pet_error::kPrivacyMode;
    err.message = text::kPrivacyMode;
    const std::string j = toJson(err);
    KV_CHECK_MSG(j == "{\"requestId\":8,\"ok\":false,\"error\":\"privacy_mode\","
                      "\"message\":\"Privacy mode is on. Turn it off on the PC or an owner's device.\"}",
                 j);
    CommandResultMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.requestId == 8 && !back.ok && back.error == std::string("privacy_mode") &&
             back.message == std::string(text::kPrivacyMode));
    KV_REQUIRE(fromJson(toJson(ok), back));
    KV_CHECK(back.ok && !back.error && !back.message);
    // A code this build doesn't know still decodes.
    KV_CHECK(fromJson(std::string("{\"requestId\":1,\"ok\":false,\"error\":\"rate_limited\"}"), back) &&
             back.error == std::string("rate_limited"));
    CommandResultMessage x;
    KV_CHECK(!fromJson(std::string("{\"requestId\":1}"), x));
    KV_CHECK(!fromJson(std::string("{\"ok\":true}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"ok\":\"true\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"ok\":false,\"error\":1}"), x));
}

KV_TEST(v3_live_view_req_json) {
    LiveViewRequestMessage stop;
    stop.watch = false;
    KV_CHECK(toJson(stop) == "{\"watch\":false}");
    LiveViewRequestMessage q;
    q.watch = true;
    q.quality = toWire(LiveQuality::Medium);
    KV_CHECK(toJson(q) == "{\"watch\":true,\"quality\":\"medium\"}");
    LiveViewRequestMessage back;
    back.quality = "stale";
    KV_REQUIRE(fromJson(toJson(stop), back));
    KV_CHECK(!back.watch && !back.quality);
    KV_REQUIRE(fromJson(std::string("{\"quality\":\"ultra\",\"watch\":true}"), back));
    KV_CHECK(back.watch && back.quality == std::string("ultra") && liveQualityFromWire(*back.quality) == LiveQuality::Unknown);
    LiveViewRequestMessage x;
    KV_CHECK(!fromJson(std::string("{\"quality\":\"auto\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"watch\":\"true\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"watch\":true,\"quality\":2}"), x));
    KV_CHECK(fromJson(std::string("{\"watch\":true,\"quality\":null}"), x) && !x.quality);
}

KV_TEST(v3_session_notice_json_and_reasons) {
    const SessionNoticeMessage revoked = makeSessionNotice(notice_reason::kRevoked);
    const std::string j = toJson(revoked);
    KV_CHECK_MSG(j == "{\"reason\":\"revoked\",\"closing\":true,\"message\":\"This device was removed from the PC.\"}", j);
    const std::string moved = toJson(makeSessionNotice(notice_reason::kAddressChanged));
    KV_CHECK_MSG(moved == "{\"reason\":\"address_changed\",\"closing\":false,\"message\":\"The PC's home address changed.\"}",
                 moved);
    SessionNoticeMessage bare;
    bare.reason = notice_reason::kReplaced;
    KV_CHECK(toJson(bare) == "{\"reason\":\"replaced\",\"closing\":true}");

    // §5.5: every reason closes except address_changed, and each has its §9 sentence.
    const std::pair<const char*, const char*> reasons[] = {
        {notice_reason::kRevoked, text::kRevoked},
        {notice_reason::kTooManySessions, text::kTooManySessions},
        {notice_reason::kReplaced, text::kReplaced},
        {notice_reason::kOutsideDisabled, text::kOutsideDisabled},
        {notice_reason::kHostShuttingDown, text::kHostShuttingDown},
        {notice_reason::kAddressChanged, text::kAddressChanged},
    };
    for (const auto& r : reasons) {
        const SessionNoticeMessage n = makeSessionNotice(r.first);
        KV_CHECK_MSG(n.reason == r.first && n.closing == (std::string(r.first) != "address_changed") &&
                         n.message == std::string(r.second),
                     r.first);
        SessionNoticeMessage back;
        KV_CHECK_MSG(fromJson(toJson(n), back) && back.reason == n.reason && back.closing == n.closing &&
                         back.message == n.message,
                     r.first);
    }
    KV_CHECK(std::string(notice_reason::kTooManySessions) == "too_many_sessions" &&
             std::string(notice_reason::kOutsideDisabled) == "outside_disabled" &&
             std::string(notice_reason::kHostShuttingDown) == "host_shutting_down");
    // An unknown reason decodes (the device shows a generic text); it gets no sentence here.
    SessionNoticeMessage future;
    KV_REQUIRE(fromJson(std::string("{\"closing\":false,\"reason\":\"solar_flare\"}"), future));
    KV_CHECK(future.reason == "solar_flare" && !future.closing && !future.message);
    KV_CHECK(!makeSessionNotice("solar_flare").message && makeSessionNotice("solar_flare").closing);
    SessionNoticeMessage x;
    KV_CHECK(!fromJson(std::string("{\"closing\":true}"), x));
    KV_CHECK(!fromJson(std::string("{\"reason\":\"revoked\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"reason\":\"revoked\",\"closing\":1}"), x));
}

KV_TEST(v3_sentences_match_the_spec) {
    // docs/protocol-v3-pet-camera.md §9, word for word.
    KV_CHECK(std::string(text::kPrivacyMode) == "Privacy mode is on. Turn it off on the PC or an owner's device.");
    KV_CHECK(std::string(text::kPcReplaying) == "The PC is playing a recording.");
    KV_CHECK(std::string(text::kSharingOff) == "Sharing is off on the PC.");
    KV_CHECK(std::string(text::kSharingSchedule) == "Sharing is off at this time of day.");
    KV_CHECK(std::string(text::kTooManyViewers) == "Four devices are already watching.");
    KV_CHECK(std::string(text::kViewerPower) == "Only an owner can turn the camera on or off.");
    KV_CHECK(std::string(text::kViewerRecording) == "Only an owner can record.");
    KV_CHECK(std::string(text::kRollDisabled) == "Recording to the camera roll is off.");
    KV_CHECK(std::string(text::kRollPreparing) == "The camera roll is getting ready.");
    KV_CHECK(std::string(text::kSensorOff) == "The camera is off.");
    KV_CHECK(std::string(text::kViewFrozen) == "The view on the PC is frozen.");
    KV_CHECK(std::string(text::kDiskFloor) == "Recording stopped: less than 20 GB is free on the PC's disk.");
    KV_CHECK(std::string(text::kStorageFull) == "Recording stopped: the camera roll is full of starred clips.");
    // The 0x17 sentences (§9), which carry no code.
    KV_CHECK(std::string(text::kClipDeleted) == "This clip was deleted.");
    KV_CHECK(std::string(text::kFolderUnavailable) == "The camera roll folder on the PC can't be used.");
    KV_CHECK(std::string(text::kClipUnreadable) == "The PC couldn't read this clip.");
    KV_CHECK(std::string(text::kPlaybackIdle) == "Playback stopped after a minute without a command.");
    KV_CHECK(std::string(text::kRevoked) == "This device was removed from the PC.");
    KV_CHECK(std::string(text::kReplaced) == "This device connected again somewhere else.");
    KV_CHECK(std::string(text::kOutsideDisabled) == "Viewing from outside the home was turned off on the PC.");
    KV_CHECK(std::string(text::kAddressChanged) == "The PC's home address changed.");
    KV_CHECK(std::string(text::kHostShuttingDown) == "The camera app on the PC was closed.");
    KV_CHECK(std::string(text::kTooManySessions) == "Too many devices are connected to this PC.");
    KV_CHECK(std::string(text::kTooManyPairedDevices) == "This PC has 16 paired devices. Revoke one first.");
    KV_CHECK(std::string(text::kInviteExpiredOrUsed) == "This invite has expired or was already used. Ask for a new one.");
    KV_CHECK(std::string(text::kInviteWrongCode) == "That invite didn't match. It can't be used again; ask for a new one.");
    KV_CHECK(std::string(text::kInviteDidntMatch) == "That invite didn't match. Check that you used the whole link you were sent.");
    KV_CHECK(std::string(text::kInviteClosed) == "This invite was closed. Ask for a new one."); // W8

    // The sentence for each code that has one: every liveUnavailableReason, every rollBlockedReason
    // (the start_clip error) and every 0x0F reason.
    const std::pair<const char*, const char*> codes[] = {
        {live_unavailable::kPrivacyMode, text::kPrivacyMode},
        {live_unavailable::kPcReplaying, text::kPcReplaying},
        {live_unavailable::kSharingOff, text::kSharingOff},
        {live_unavailable::kSharingSchedule, text::kSharingSchedule},
        {live_unavailable::kTooManyViewers, text::kTooManyViewers},
        {roll_blocked::kPrivacyMode, text::kPrivacyMode},
        {roll_blocked::kRollDisabled, text::kRollDisabled},
        {roll_blocked::kRollPreparing, text::kRollPreparing},
        {roll_blocked::kFolderUnavailable, text::kFolderUnavailable},
        {roll_blocked::kSensorOff, text::kSensorOff},
        {roll_blocked::kViewFrozen, text::kViewFrozen},
        {roll_blocked::kPcReplaying, text::kPcReplaying},
        {roll_blocked::kDiskFloor, text::kDiskFloor},
        {roll_blocked::kStorageFull, text::kStorageFull},
        {notice_reason::kRevoked, text::kRevoked},
        {notice_reason::kTooManySessions, text::kTooManySessions},
        {notice_reason::kReplaced, text::kReplaced},
        {notice_reason::kOutsideDisabled, text::kOutsideDisabled},
        {notice_reason::kHostShuttingDown, text::kHostShuttingDown},
        {notice_reason::kAddressChanged, text::kAddressChanged},
    };
    for (const auto& c : codes) {
        const char* s = text::sentenceFor(c.first);
        KV_CHECK_MSG(s && std::string(s) == c.second, c.first);
        KV_CHECK_MSG(pet_error::isKnown(c.first) || std::string(c.first) == "revoked" ||
                         std::string(c.first) == "too_many_sessions" || std::string(c.first) == "replaced" ||
                         std::string(c.first) == "outside_disabled" || std::string(c.first) == "host_shutting_down" ||
                         std::string(c.first) == "address_changed",
                     c.first);
    }
    for (const char* none : {"not_allowed_for_role", "not_found", "busy", "invalid", "unsupported", "internal", "", "x"}) {
        KV_CHECK_MSG(text::sentenceFor(none) == nullptr, none);
    }
    KV_CHECK(std::string(outside_status::kCarrierNat) == "carrier_nat" &&
             std::string(outside_status::kAddressChanged) == "address_changed");
}

KV_TEST(hkdf_is_rfc5869) {
    Bytes okm;
    KV_REQUIRE(hkdfSha256(Bytes(22, 0x0B), Bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
                          Bytes{0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9}, 42, okm));
    KV_CHECK(okm == B(kRfc5869Okm, kRfc5869Okm_len));
}

KV_TEST(pairing_proofs_match_swift_vectors) {
    const Bytes sfp = B(kGoldenServerFp, kGoldenServerFp_len);
    const Bytes cfp = B(kGoldenClientFp, kGoldenClientFp_len);
    const Bytes tok = B(kGoldenQrToken, kGoldenQrToken_len);
    const std::string code = "12345678";
    const Bytes code_secret(code.begin(), code.end());
    KV_CHECK(computePairingProof(tok, sfp, cfp, "kv2-pair-client") == B(kGoldenProofQrClient, kGoldenProofQrClient_len));
    KV_CHECK(computePairingProof(tok, sfp, cfp, "kv2-pair-server") == B(kGoldenProofQrServer, kGoldenProofQrServer_len));
    KV_CHECK(computePairingProof(code_secret, sfp, cfp, "kv2-pair-client") ==
             B(kGoldenProofCodeClient, kGoldenProofCodeClient_len));
    KV_CHECK(computePairingProof(code_secret, sfp, cfp, "kv2-pair-server") ==
             B(kGoldenProofCodeServer, kGoldenProofCodeServer_len));
}

KV_TEST(constant_time_equal_has_swift_semantics) {
    KV_CHECK(constantTimeEqual(Bytes{1, 2, 3}, Bytes{1, 2, 3}));
    KV_CHECK(!constantTimeEqual(Bytes{1, 2, 3}, Bytes{1, 2, 4}));
    KV_CHECK(!constantTimeEqual(Bytes{1, 2, 3}, Bytes{1, 2}));
    KV_CHECK(!constantTimeEqual(Bytes{}, Bytes{}));
}

KV_TEST(spki_and_tbs_match_swift_asn1_builder) {
    const Bytes scalar = B(kGoldenTestScalar, kGoldenTestScalar_len);
    PkeyPtr key = p256KeyFromScalar(scalar);
    KV_REQUIRE(key != nullptr);
    KV_CHECK(subjectPublicKeyInfoDer(key.get()) == B(kGoldenSpki, kGoldenSpki_len));
    const Bytes tbs = buildTbsCertificate(key.get(), B(kGoldenSerialRawBeforeFixup, kGoldenSerialRawBeforeFixup_len), "kv2-host");
    KV_CHECK_MSG(tbs == B(kGoldenTbs, kGoldenTbs_len), hex(tbs));
    Bytes raw;
    KV_CHECK(p256ScalarOf(key.get(), raw) && raw == scalar);
}

KV_TEST(generated_identity_is_a_valid_self_signed_cert_openssl_accepts) {
    std::string err;
    auto id = CryptoIdentity::generate("kv2-host", &err);
    KV_REQUIRE(id != nullptr);
    KV_CHECK(id->privateKeyRaw().size() == 32);
    KV_CHECK(id->certificateSHA256() == sha256(id->certificateDER()));
    const unsigned char* p = id->certificateDER().data();
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(id->certificateDER().size()));
    KV_REQUIRE(cert != nullptr);
    KV_CHECK(X509_verify(cert, id->key()) == 1); // signature over the TBS checks out
    KV_CHECK(X509_get_version(cert) == 2);
    X509_NAME* subj = X509_get_subject_name(cert);
    char cn[64] = {0};
    X509_NAME_get_text_by_NID(subj, NID_commonName, cn, sizeof(cn));
    KV_CHECK(std::string(cn) == "kv2-host");
    KV_CHECK((X509_get_extended_key_usage(cert) & (XKU_SSL_SERVER | XKU_SSL_CLIENT)) == (XKU_SSL_SERVER | XKU_SSL_CLIENT));
    X509_free(cert);

    auto again = CryptoIdentity::fromRecord(id->privateKeyRaw(), id->certificateDER(), &err);
    KV_REQUIRE(again != nullptr);
    KV_CHECK(again->certificateSHA256() == id->certificateSHA256());
    auto other = CryptoIdentity::generate("kv2-host");
    KV_CHECK(CryptoIdentity::fromRecord(other->privateKeyRaw(), id->certificateDER(), &err) == nullptr);
}

// ---- PairingPayload.swift ---------------------------------------------------------------------

KV_TEST(qr_uri_matches_swift_urlcomponents_output) {
    QRPairingPayload q;
    q.serviceId = "a1b2c3d4e5f6";
    for (int i = 0; i < 32; ++i) q.serverCertSHA256.push_back(static_cast<uint8_t>(i));
    for (int i = 0; i < 32; ++i) q.oneTimeToken.push_back(static_cast<uint8_t>(0xFF - i));
    q.hostIP = "192.168.1.20";
    q.pairPort = 51234;
    KV_CHECK_MSG(q.toURIString() == kGoldenQrUri, q.toURIString());
    QRPairingPayload back;
    KV_REQUIRE(QRPairingPayload::parse(std::string("  ") + kGoldenQrUri + "\n", back));
    KV_CHECK(back.serviceId == q.serviceId && back.serverCertSHA256 == q.serverCertSHA256 &&
             back.oneTimeToken == q.oneTimeToken && back.hostIP == q.hostIP && back.pairPort == q.pairPort);
}

KV_TEST(qr_parse_rejects_what_swift_rejects) {
    QRPairingPayload q;
    KV_CHECK(!QRPairingPayload::parse("https://pair?s=a&fp=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&tok=AAAAAAAAAAAAAAAAAAAAAA&h=1.2.3.4&p=1", q));
    KV_CHECK(!QRPairingPayload::parse("kv2://pair?s=a&fp=AAEC&tok=AAAAAAAAAAAAAAAAAAAAAA&h=1.2.3.4&p=1", q));        // fp != 32 B
    KV_CHECK(!QRPairingPayload::parse("kv2://pair?s=a&fp=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&tok=AAAA&h=1.2.3.4&p=1", q)); // tok < 16
    KV_CHECK(!QRPairingPayload::parse("kv2://pair?s=a&fp=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&tok=AAAAAAAAAAAAAAAAAAAAAA&h=1.2.3.4&p=0", q));
    KV_CHECK(!QRPairingPayload::parse("kv2://pair?fp=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&tok=AAAAAAAAAAAAAAAAAAAAAA&h=1.2.3.4&p=9", q));
    KV_CHECK(QRPairingPayload::parse("kv2://pair?s=a&fp=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&tok=AAAAAAAAAAAAAAAAAAAAAA&h=1.2.3.4&p=9", q));
}

KV_TEST(short_code_matches_swift) {
    const uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
    std::string formatted, normalized;
    shortCodeFromBytes(b, formatted, normalized);
    KV_CHECK(normalized == kGoldenShortCodeDigits);
    KV_CHECK(formatted == kGoldenShortCodeFormatted);
    KV_CHECK(normalizeShortCode(" 0541-9896 ") == "05419896");
    std::string f2, n2;
    KV_REQUIRE(generateShortCode(f2, n2));
    KV_CHECK(n2.size() == 8 && f2.size() == 9 && f2[4] == '-');
    const std::string id = randomHexId(6);
    KV_CHECK(id.size() == 12 && id.find_first_not_of("0123456789abcdef") == std::string::npos);
}

// ---- Host state -------------------------------------------------------------------------------

KV_TEST(host_state_json_round_trip_and_owner_only_acl) {
    HostPersistedState st;
    st.privateKeyRaw = Bytes(32, 0x42);
    st.certificateDER = Bytes{0x30, 0x03, 0x02, 0x01, 0xFF};
    st.serviceId = "0a1b2c3d4e5f";
    st.allowPhoneModeSwitch = false;
    const std::string unpaired = toJson(st);
    KV_CHECK(unpaired.find("pinnedClientCertSHA256") == std::string::npos); // nil is omitted, as Swift does
    KV_CHECK(unpaired.rfind("{\"serverIdentity\":{\"privateKeyRaw\":", 0) == 0);
    st.pinnedClientCertSHA256 = Bytes(32, 0xAB);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"kv-host-state-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\.host-state";
    std::string err;
    KV_REQUIRE(saveHostStateAtomically(dir, st, &err));
    KV_CHECK(isOwnerOnly(dir));
    KV_CHECK(isOwnerOnly(dir + L"\\pairing.json"));
    HostPersistedState back;
    KV_REQUIRE(loadHostState(dir, back));
    KV_CHECK(back.privateKeyRaw == st.privateKeyRaw && back.certificateDER == st.certificateDER &&
             back.serviceId == st.serviceId && back.pinnedClientCertSHA256 == st.pinnedClientCertSHA256 &&
             back.allowPhoneModeSwitch == false);
    DeleteFileW((dir + L"\\pairing.json").c_str());
    RemoveDirectoryW(dir.c_str());
}

KV_TEST(host_state_power_settings_persist_and_default_for_older_files) {
    HostPersistedState st;
    st.privateKeyRaw = Bytes(32, 0x42);
    st.certificateDER = Bytes{0x30, 0x03, 0x02, 0x01, 0xFF};
    st.serviceId = "0a1b2c3d4e5f";
    KV_CHECK(st.allowPhoneSensorPowerControl && !st.turnOffKinectOnPhoneDisconnect); // on, and off, by default
    st.allowPhoneSensorPowerControl = false;
    st.turnOffKinectOnPhoneDisconnect = true;
    const std::string j = toJson(st);
    KV_CHECK(j.find(",\"allowPhoneModeSwitch\":true,\"allowPhoneSensorPowerControl\":false,"
                    "\"turnOffKinectOnPhoneDisconnect\":true}") != std::string::npos);
    HostPersistedState back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(!back.allowPhoneSensorPowerControl && back.turnOffKinectOnPhoneDisconnect);

    // A pairing.json written before phase 7 has neither key: power control on, turn-off off.
    const std::string before = "{\"serverIdentity\":{\"privateKeyRaw\":\"QkJC\",\"certificateDER\":\"MAMCAf8=\"},"
                               "\"serviceId\":\"0a1b2c3d4e5f\",\"allowPhoneModeSwitch\":false}";
    HostPersistedState old;
    old.allowPhoneSensorPowerControl = false;
    old.turnOffKinectOnPhoneDisconnect = true;
    KV_REQUIRE(fromJson(before, old));
    KV_CHECK(old.allowPhoneSensorPowerControl && !old.turnOffKinectOnPhoneDisconnect && !old.allowPhoneModeSwitch);
}

// ---- The host key at rest (wrapped with DPAPI for this PC) ---------------------------------------------------

namespace {

std::wstring keyStateDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"kv-host-key-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) +
                             L"\\.host-state";
    for (const wchar_t* f : {L"pairing.json", L"pairing.json.tmp", L"pairing.json.prev", L"pairing.json.bad", L"pairing.json.bad.1"}) {
        DeleteFileW((dir + L"\\" + f).c_str());
    }
    return dir;
}

std::string pairingText(const std::wstring& dir) {
    std::string t;
    readWholeFile(dir + L"\\pairing.json", t);
    return t;
}

// The base64 that pairing.json would hold for key if it were written plain.
std::string keyBase64(const Bytes& key) {
    const std::string j = json::ObjectWriter().bytes("k", key).finish(); // {"k":"..."}
    return j.substr(6, j.size() - 8);
}

// A pairing.json as a build before the wrap wrote it: the key plain.
HostPersistedState plainPairing(const std::wstring& dir) {
    std::string e;
    auto id = CryptoIdentity::generate(newHostCommonName(), &e);
    HostPersistedState st;
    if (!id) return st;
    st.privateKeyRaw = id->privateKeyRaw();
    st.certificateDER = id->certificateDER();
    st.serviceId = randomHexId(6);
    ensureOwnerOnlyDirectory(dir);
    writeOwnerOnlyFileAtomically(dir, L"pairing.json", toJson(st)); // no privateKeyWrapped: written plain
    return st;
}

// No pairing.json.tmp* left in dir: neither an earlier build's name nor a process's pairing.json.tmp.<pid>.
bool noPairingTmp(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\pairing.json.tmp*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    FindClose(h);
    return false;
}

// A row's temp folder (keyStateDir's parent) goes when the row ends, however it ends.
struct KeyDirCleanup {
    std::wstring dir;
    ~KeyDirCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(dir).parent_path(), ec);
    }
};

// DPAPI made to fail, for one row, and put back when it ends.
struct DpapiErrors {
    DpapiErrors(unsigned long wrap, unsigned long unwrap) { setHostKeyDpapiErrorsForTest(wrap, unwrap); }
    ~DpapiErrors() { setHostKeyDpapiErrorsForTest(0, 0); }
};

} // namespace

// The host key at rest: a pairing.json from before the wrap is migrated by the first start, and afterwards the key is
// never on disk plain: not in pairing.json (neither its field nor its base64), not in a .tmp left behind, and
// not after a later save. The key is the same one (the identity every device pinned), and loads.
KV_TEST(host_key_is_never_plain_on_disk_after_migration) {
    const std::wstring dir = keyStateDir(L"migrate");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState st = plainPairing(dir);
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    const std::string b64 = keyBase64(st.privateKeyRaw);
    KV_REQUIRE(pairingText(dir).find(b64) != std::string::npos); // it starts plain
    std::string note;
    KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note);
    const std::string after = pairingText(dir);
    KV_CHECK_MSG(after.find("privateKeyRaw") == std::string::npos, after);
    KV_CHECK_MSG(after.find(b64) == std::string::npos, "the key's base64 is still in pairing.json");
    KV_CHECK(after.find("\"privateKeyWrapped\":") != std::string::npos);
    KV_CHECK(noPairingTmp(dir));
    KV_CHECK(isOwnerOnly(dir + L"\\pairing.json"));
    HostPersistedState back;
    KV_REQUIRE(loadHostState(dir, back));
    KV_CHECK(back.privateKeyRaw == st.privateKeyRaw && back.certificateDER == st.certificateDER &&
             back.serviceId == st.serviceId);
    KV_CHECK(CryptoIdentity::fromRecord(back.privateKeyRaw, back.certificateDER) != nullptr);
    // A later save (a setting, a pairing) writes it wrapped again, never plain.
    back.allowPhoneModeSwitch = false;
    std::string err;
    const bool saved_ok = saveHostStateAtomically(dir, back, &err);
    KV_CHECK_MSG(saved_ok, err);
    KV_REQUIRE(saved_ok);
    const std::string saved = pairingText(dir);
    KV_CHECK(saved.find("privateKeyRaw") == std::string::npos && saved.find(b64) == std::string::npos);
    HostPersistedState again;
    KV_REQUIRE(loadHostState(dir, again));
    KV_CHECK(again.privateKeyRaw == st.privateKeyRaw && !again.allowPhoneModeSwitch);
}

// The host key at rest: a wrapped key that won't unwrap on this PC (damaged here; wrapped on another PC or
// Windows install fails the same way) makes pairing.json unusable, never missing: the load says so and names
// the reset, and writes nothing; the migration leaves it alone. The owner's reset keeps it as
// pairing.json.bad and makes a new identity, itself wrapped.
KV_TEST(a_host_key_that_wont_unwrap_fails_closed_and_the_reset_makes_a_new_one) {
    const std::wstring dir = keyStateDir(L"unwrap");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState st = plainPairing(dir);
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    std::string err;
    const bool saved_ok = saveHostStateAtomically(dir, st, &err);
    KV_CHECK_MSG(saved_ok, err);
    KV_REQUIRE(saved_ok);
    HostPersistedState onDisk;
    KV_REQUIRE(fromJson(pairingText(dir), onDisk) && onDisk.privateKeyRaw.empty() &&
               onDisk.privateKeyWrapped.size() > 16);
    onDisk.privateKeyWrapped[onDisk.privateKeyWrapped.size() - 5] ^= 0x01; // one bit of the wrap's signature
    const std::string damaged = toJson(onDisk);
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json", damaged));
    HostPersistedState s;
    const HostStateLoad load = loadHostStateChecked(dir, s, &err);
    KV_CHECK_MSG(load == HostStateLoad::Unusable, err);
    KV_CHECK_MSG(err.find("can't be unwrapped") != std::string::npos &&
                     err.find("Reset this PC's identity") != std::string::npos,
                 err);
    KV_CHECK(pairingText(dir) == damaged);
    std::string note;
    KV_CHECK(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::AlreadyWrapped);
    KV_CHECK(pairingText(dir) == damaged);
    KV_CHECK_MSG(resetHostIdentity(dir, &err, &note) == IdentityReset::Done, err);
    std::string bad;
    KV_CHECK(readWholeFile(dir + L"\\pairing.json.bad", bad) && bad == damaged);
    HostPersistedState fresh;
    KV_REQUIRE(loadHostState(dir, fresh));
    KV_CHECK(fresh.privateKeyRaw != st.privateKeyRaw && fresh.serviceId != st.serviceId);
    KV_CHECK(pairingText(dir).find("privateKeyRaw") == std::string::npos);
}

// The host key at rest: migrating is idempotent. A second run over a wrapped file writes nothing (the file is
// byte for byte the same; a rewrite would differ, since every wrap does). A file left with both keys (a start
// stopped between the two writes) is finished from its plain key, even when the wrapped one beside it is
// another key's.
KV_TEST(host_key_migration_is_idempotent) {
    const std::wstring dir = keyStateDir(L"idem");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState st = plainPairing(dir);
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    std::string note;
    const bool migrated = migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated;
    KV_CHECK_MSG(migrated, note);
    KV_REQUIRE(migrated);
    const std::string once = pairingText(dir);
    KV_CHECK(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::AlreadyWrapped);
    KV_CHECK_MSG(pairingText(dir) == once, "a second migration rewrote pairing.json");
    HostPersistedState wrapped;
    KV_REQUIRE(fromJson(once, wrapped));
    // Both keys, the wrapped one another identity's.
    std::string e;
    auto other = CryptoIdentity::generate(newHostCommonName(), &e);
    KV_REQUIRE(other);
    const std::wstring otherDir = keyStateDir(L"idem-other");
    const KeyDirCleanup cleanupOther{otherDir};
    HostPersistedState o;
    o.privateKeyRaw = other->privateKeyRaw();
    o.certificateDER = other->certificateDER();
    o.serviceId = randomHexId(6);
    KV_REQUIRE(saveHostStateAtomically(otherDir, o, &e));
    HostPersistedState oDisk;
    KV_REQUIRE(fromJson(pairingText(otherDir), oDisk));
    HostPersistedState both = st;
    both.privateKeyWrapped = oDisk.privateKeyWrapped;
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json", toJson(both)));
    KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note);
    HostPersistedState back;
    KV_REQUIRE(loadHostState(dir, back));
    KV_CHECK(back.privateKeyRaw == st.privateKeyRaw);
    KV_CHECK(pairingText(dir).find("privateKeyRaw") == std::string::npos);
    const std::string twice = pairingText(dir);
    KV_CHECK(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::AlreadyWrapped && pairingText(dir) == twice);
}

// The host key at rest: the plain key is removed only once the wrapped one reads back from disk. When it
// doesn't, pairing.json still holds the plain key, the identity loads as before (the host starts:
// reachability first), and the next start migrates it.
KV_TEST(a_wrapped_key_that_doesnt_read_back_keeps_the_plain_one) {
    const std::wstring dir = keyStateDir(L"readback");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState st = plainPairing(dir);
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    std::string note;
    auto never = [](const std::vector<uint8_t>&, std::vector<uint8_t>& key) {
        key.clear();
        return false;
    };
    KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note, never) == HostKeyMigration::KeptPlain, note);
    KV_CHECK_MSG(note.find("stays plain") != std::string::npos, note);
    HostPersistedState kept;
    KV_REQUIRE(fromJson(pairingText(dir), kept));
    KV_CHECK(kept.privateKeyRaw == st.privateKeyRaw); // the plain key is still on disk
    HostPersistedState back;
    KV_REQUIRE(loadHostState(dir, back));
    KV_CHECK(back.privateKeyRaw == st.privateKeyRaw &&
             CryptoIdentity::fromRecord(back.privateKeyRaw, back.certificateDER) != nullptr);
    KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note); // the next start
    KV_CHECK(pairingText(dir).find("privateKeyRaw") == std::string::npos);
}

// The host key at rest: a key that won't unwrap is told by why. A data error (the wrap damaged, made with other
// entropy, cut short, or from another PC or Windows install) points to the reset; any other error may pass, so the
// load says to try again first (a reset would make every device pair again for nothing) and says it may pass.
KV_TEST(a_key_that_wont_unwrap_is_told_by_why) {
    // The codes DPAPI gives, measured: 13 for damage, other entropy or garbage; 87 for a wrap cut short;
    // NTE_BAD_KEY_STATE for a master key this PC hasn't got. Out of memory and an RPC failure aren't data errors.
    for (const unsigned long data : {13ul, 87ul, static_cast<unsigned long>(NTE_BAD_KEY_STATE),
                                     static_cast<unsigned long>(NTE_BAD_DATA), static_cast<unsigned long>(NTE_BAD_KEY)})
        KV_CHECK_MSG(hostKeyUnwrapErrorIsData(data), std::to_string(data));
    for (const unsigned long passing : {8ul, 14ul, 1722ul, 1726ul, 0ul})
        KV_CHECK_MSG(!hostKeyUnwrapErrorIsData(passing), std::to_string(passing));

    const std::wstring dir = keyStateDir(L"why");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState st = plainPairing(dir);
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    std::string err;
    KV_REQUIRE(saveHostStateAtomically(dir, st, &err));
    HostPersistedState wrapped;
    KV_REQUIRE(fromJson(pairingText(dir), wrapped) && !wrapped.privateKeyWrapped.empty());
    auto loadOf = [&](const HostPersistedState& onDisk, bool* passing) -> std::string {
        KV_CHECK(writeOwnerOnlyFileAtomically(dir, L"pairing.json", toJson(onDisk))); // (no REQUIRE in a lambda)
        HostPersistedState s;
        const HostStateLoad load = loadHostStateChecked(dir, s, &err, passing);
        KV_CHECK_MSG(load == HostStateLoad::Unusable, err);
        return err;
    };
    // Data errors: one bit of the wrap changed; the key wrapped with another app's entropy; the wrap cut short.
    HostPersistedState damaged = wrapped;
    damaged.privateKeyWrapped[damaged.privateKeyWrapped.size() - 5] ^= 0x01;
    HostPersistedState otherEntropy = wrapped;
    {
        const char other[] = "some other app";
        DATA_BLOB in{static_cast<DWORD>(st.privateKeyRaw.size()), const_cast<BYTE*>(st.privateKeyRaw.data())};
        DATA_BLOB entropy{static_cast<DWORD>(sizeof(other) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(other))};
        DATA_BLOB blob{};
        KV_REQUIRE(CryptProtectData(&in, nullptr, &entropy, nullptr, nullptr,
                                    CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &blob));
        otherEntropy.privateKeyWrapped.assign(blob.pbData, blob.pbData + blob.cbData);
        LocalFree(blob.pbData);
    }
    HostPersistedState cutShort = wrapped;
    cutShort.privateKeyWrapped.resize(40);
    for (const HostPersistedState* bad : {&damaged, &otherEntropy, &cutShort}) {
        bool passing = true;
        const std::string why = loadOf(*bad, &passing);
        KV_CHECK_MSG(!passing, why);
        KV_CHECK_MSG(why.find("Reset this PC's identity to make a new one") != std::string::npos, why);
    }
    // Errors that may pass (out of memory, the DPAPI service not answering), made to happen here.
    for (const unsigned long passingError : {8ul, 1722ul}) {
        const DpapiErrors fail(0, passingError);
        bool passing = false;
        const std::string why = loadOf(wrapped, &passing);
        KV_CHECK_MSG(passing, why);
        KV_CHECK_MSG(why.find("try again") != std::string::npos, why);
        KV_CHECK_MSG(why.find("to make a new one") == std::string::npos, why);
        KV_CHECK(pairingText(dir) == toJson(wrapped)); // nothing written
    }
    HostPersistedState back; // and once it passes, the same key loads
    KV_CHECK(loadHostState(dir, back) && back.privateKeyRaw == st.privateKeyRaw);
}

// The host key at rest: with DPAPI not working, a save keeps a plain pairing.json plain (or writes a first run's
// plain), so a phone still pairs and a setting still saves; reachability first, as the migration does. Over a
// wrapped pairing.json it never writes the key plain: the save fails and the file is as it was. Once DPAPI works,
// the migration wraps the plain one.
KV_TEST(a_save_that_cant_wrap_keeps_a_plain_file_plain_and_never_unwraps_a_wrapped_one) {
    const std::wstring dir = keyStateDir(L"nowrap");
    const KeyDirCleanup cleanup{dir};
    HostPersistedState st = plainPairing(dir); // plain on disk
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    std::string err;
    {
        const DpapiErrors fail(1722, 0);
        st.allowPhoneModeSwitch = false; // a setting changed
        const bool saved = saveHostStateAtomically(dir, st, &err);
        KV_CHECK_MSG(saved, err);
        HostPersistedState onDisk;
        KV_REQUIRE(fromJson(pairingText(dir), onDisk));
        KV_CHECK(onDisk.privateKeyRaw == st.privateKeyRaw && onDisk.privateKeyWrapped.empty() &&
                 !onDisk.allowPhoneModeSwitch);
        // A first run: no pairing.json yet.
        const std::wstring fresh = keyStateDir(L"nowrap-first");
        const KeyDirCleanup cleanupFresh{fresh};
        KV_CHECK_MSG(saveHostStateAtomically(fresh, st, &err), err);
        HostPersistedState first;
        KV_CHECK(fromJson(pairingText(fresh), first) && first.privateKeyRaw == st.privateKeyRaw);
    }
    // Wrapped now (DPAPI works again): the next start's migration.
    std::string note;
    KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note);
    const std::string wrappedText = pairingText(dir);
    KV_REQUIRE(wrappedText.find("privateKeyRaw") == std::string::npos);
    {
        const DpapiErrors fail(8, 0);
        st.allowPhoneModeSwitch = true;
        KV_CHECK_MSG(!saveHostStateAtomically(dir, st, &err),
                     "a wrapped pairing.json was saved over with the key plain");
        KV_CHECK(pairingText(dir) == wrappedText);
    }
}

// Another start of the host on the same folder can wrap the key between a plain save's check and its write (its
// migration, run here from the test hook). The write finds the wrap in what it replaced, puts it back as it was,
// and the save fails: a plain key never lands over a fresh wrap. A first run's plain save never replaces a
// pairing.json written meanwhile either. With nothing in between, the plain save goes through and leaves nothing.
KV_TEST(a_plain_save_never_lands_over_a_wrap_made_meanwhile) {
    const std::wstring dir = keyStateDir(L"plainrace");
    const KeyDirCleanup cleanup{dir};
    HostPersistedState st = plainPairing(dir); // plain on disk
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    const auto gone = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; };
    std::string wrappedText;
    {
        const DpapiErrors fail(1722, 0);
        setHostKeyBeforePlainSaveForTest([&] {
            setHostKeyDpapiErrorsForTest(0, 0); // the other start's DPAPI works
            std::string note;
            KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note);
            wrappedText = pairingText(dir);
            setHostKeyDpapiErrorsForTest(1722, 0);
        });
        st.allowPhoneModeSwitch = !st.allowPhoneModeSwitch; // a setting changed
        std::string err;
        const bool saved = saveHostStateAtomically(dir, st, &err);
        setHostKeyBeforePlainSaveForTest(nullptr);
        KV_CHECK_MSG(!saved, "a plain save landed over a wrap made meanwhile");
        KV_CHECK_MSG(err.find("was wrapped") != std::string::npos && err.find("back as it was") != std::string::npos, err);
    }
    KV_REQUIRE(wrappedText.find("privateKeyWrapped") != std::string::npos);
    KV_CHECK(pairingText(dir) == wrappedText);
    KV_CHECK(pairingText(dir).find(keyBase64(st.privateKeyRaw)) == std::string::npos);
    KV_CHECK(gone(dir + L"\\pairing.json.prev") && noPairingTmp(dir));
    KV_CHECK(isOwnerOnly(dir + L"\\pairing.json"));

    // A first run: a pairing.json written between the check and the write is never replaced.
    const std::wstring first = keyStateDir(L"plainrace-first");
    const KeyDirCleanup cleanupFirst{first};
    const std::string other = "{\"written\":\"meanwhile\"}";
    {
        const DpapiErrors fail(1722, 0);
        setHostKeyBeforePlainSaveForTest([&] {
            ensureOwnerOnlyDirectory(first);
            writeOwnerOnlyFileAtomically(first, L"pairing.json", other);
        });
        std::string err;
        const bool saved = saveHostStateAtomically(first, st, &err);
        setHostKeyBeforePlainSaveForTest(nullptr);
        KV_CHECK_MSG(!saved, "a first run's plain save replaced a pairing.json written meanwhile");
        KV_CHECK_MSG(err.find("written while") != std::string::npos, err);
    }
    KV_CHECK(pairingText(first) == other);
    KV_CHECK(noPairingTmp(first));

    // Nothing in between: the plain save goes through, plain, owner-only, with no .prev or .tmp left.
    const std::wstring quiet = keyStateDir(L"plainrace-quiet");
    const KeyDirCleanup cleanupQuiet{quiet};
    HostPersistedState q = plainPairing(quiet);
    KV_REQUIRE(q.privateKeyRaw.size() == 32);
    {
        const DpapiErrors fail(1722, 0);
        q.allowPhoneModeSwitch = !q.allowPhoneModeSwitch;
        std::string err;
        KV_CHECK_MSG(saveHostStateAtomically(quiet, q, &err), err);
    }
    HostPersistedState onDisk;
    KV_REQUIRE(fromJson(pairingText(quiet), onDisk));
    KV_CHECK(onDisk.privateKeyRaw == q.privateKeyRaw && onDisk.allowPhoneModeSwitch == q.allowPhoneModeSwitch);
    KV_CHECK(gone(quiet + L"\\pairing.json.prev") && noPairingTmp(quiet));
    KV_CHECK(isOwnerOnly(quiet + L"\\pairing.json"));
}

// A plain save's replace can fail midway (an antivirus or indexer holding the file): Windows may have moved
// pairing.json to the backup name and not moved the new file in (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2). pairing.json
// is back from .prev before the save returns, so the folder is never without it and no start reads it as a first
// run. The same when putting back a wrap made meanwhile fails midway (no backup name: the replaced file is gone).
KV_TEST(a_replace_that_fails_midway_never_leaves_the_folder_without_pairing_json) {
    const std::wstring dir = keyStateDir(L"replacefault");
    const KeyDirCleanup cleanup{dir};
    HostPersistedState st = plainPairing(dir); // plain on disk
    KV_REQUIRE(st.privateKeyRaw.size() == 32);
    const auto gone = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; };
    const std::string original = pairingText(dir);
    {
        const DpapiErrors fail(1722, 0);
        setHostKeyReplaceFaultForTest(1, ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        st.allowPhoneModeSwitch = !st.allowPhoneModeSwitch;
        std::string err;
        const bool saved = saveHostStateAtomically(dir, st, &err);
        setHostKeyReplaceFaultForTest(0, 0);
        KV_CHECK_MSG(!saved, "a save whose replace failed midway said it saved");
        KV_CHECK_MSG(err.find("1177") != std::string::npos && err.find("is as it was") != std::string::npos, err);
    }
    KV_CHECK_MSG(pairingText(dir) == original, "pairing.json isn't the file the failed replace had moved away");
    KV_CHECK(gone(dir + L"\\pairing.json.prev") && noPairingTmp(dir));
    HostPersistedState back;
    KV_CHECK(loadHostStateChecked(dir, back) == HostStateLoad::Loaded && back.privateKeyRaw == st.privateKeyRaw);

    std::string wrappedText;
    {
        const DpapiErrors fail(1722, 0);
        setHostKeyBeforePlainSaveForTest([&] {
            setHostKeyDpapiErrorsForTest(0, 0); // the other start's DPAPI works
            std::string note;
            KV_CHECK_MSG(migrateHostKeyToWrapped(dir, &note) == HostKeyMigration::Migrated, note);
            wrappedText = pairingText(dir);
            setHostKeyDpapiErrorsForTest(1722, 0);
        });
        setHostKeyReplaceFaultForTest(2, ERROR_UNABLE_TO_MOVE_REPLACEMENT); // the second replace: putting it back
        std::string err;
        const bool saved = saveHostStateAtomically(dir, st, &err);
        setHostKeyBeforePlainSaveForTest(nullptr);
        setHostKeyReplaceFaultForTest(0, 0);
        KV_CHECK_MSG(!saved, "a plain save landed over a wrap made meanwhile");
        KV_CHECK_MSG(err.find("back as it was") != std::string::npos, err);
    }
    KV_REQUIRE(wrappedText.find("privateKeyWrapped") != std::string::npos);
    KV_CHECK_MSG(pairingText(dir) == wrappedText, "the wrap made meanwhile isn't pairing.json again");
    KV_CHECK(gone(dir + L"\\pairing.json.prev") && noPairingTmp(dir));
}

// A pairing.json.prev beside a pairing.json is removed only when it is the same identity (the same key, plain or
// unwrapped): the file a save replaced. Another identity (a reset another start made meanwhile, whose put-back failed
// with both files left) may be the only copy of it, so it is kept as pairing.json.prev.bad before .prev goes.
KV_TEST(a_leftover_prev_beside_pairing_json_is_removed_only_when_it_is_the_same_identity) {
    const std::wstring dir = keyStateDir(L"prevsame");
    const KeyDirCleanup cleanup{dir};
    const HostPersistedState a = plainPairing(dir); // pairing.json: identity A, plain
    KV_REQUIRE(a.privateKeyRaw.size() == 32);
    const std::string aText = pairingText(dir);
    const auto gone = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; };
    // The file a save replaced: A, a setting apart. Removed, with no .bad copy.
    HostPersistedState a2 = a;
    a2.allowPhoneModeSwitch = !a.allowPhoneModeSwitch;
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json.prev", toJson(a2)));
    std::string note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("the same identity") != std::string::npos, note);
    KV_CHECK(gone(dir + L"\\pairing.json.prev") && gone(dir + L"\\pairing.json.prev.bad"));
    KV_CHECK(pairingText(dir) == aText);
    // Another identity: kept as pairing.json.prev.bad, then .prev goes; pairing.json is as it was.
    const std::wstring other = keyStateDir(L"prevother");
    const KeyDirCleanup cleanupOther{other};
    const HostPersistedState b = plainPairing(other);
    KV_REQUIRE(b.privateKeyRaw.size() == 32 && b.privateKeyRaw != a.privateKeyRaw);
    const std::string bText = pairingText(other);
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json.prev", bText));
    note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("another identity") != std::string::npos, note);
    std::string kept;
    KV_CHECK_MSG(readWholeFile(dir + L"\\pairing.json.prev.bad", kept) && kept == bText,
                 "another identity's .prev was removed without a .bad copy");
    KV_CHECK(gone(dir + L"\\pairing.json.prev"));
    KV_CHECK(pairingText(dir) == aText);
    // With pairing.json wrapped, a plain .prev of the same key compares by the unwrapped key: removed.
    std::string wrapNote;
    KV_REQUIRE(migrateHostKeyToWrapped(dir, &wrapNote) == HostKeyMigration::Migrated);
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json.prev", aText));
    note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("the same identity") != std::string::npos, note);
    KV_CHECK(gone(dir + L"\\pairing.json.prev"));
}

// What an interrupted write left, at a start (recoverPairingLeftovers): a pairing.json.prev with no pairing.json is
// pairing.json again; beside one it's removed. Temp files of writes that never finished (an earlier build's name, a
// process that is gone) are removed; a running process's, and this one's, stay.
KV_TEST(an_interrupted_writes_leftovers_are_sorted_out_and_a_running_writes_temp_stays) {
    const std::wstring dir = keyStateDir(L"leftovers");
    const KeyDirCleanup cleanup{dir};
    KV_REQUIRE(ensureOwnerOnlyDirectory(dir));
    const auto gone = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; };
    KV_CHECK(recoverPairingLeftovers(dir).empty()); // nothing there: nothing done
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json.prev", "{\"was\":1}"));
    std::string note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("pairing.json again") != std::string::npos, note);
    KV_CHECK(pairingText(dir) == "{\"was\":1}" && gone(dir + L"\\pairing.json.prev"));
    KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, L"pairing.json.prev", "{\"older\":1}"));
    note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("is removed") != std::string::npos, note);
    KV_CHECK(pairingText(dir) == "{\"was\":1}" && gone(dir + L"\\pairing.json.prev"));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
    KV_REQUIRE(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);
    const std::wstring running = dir + L"\\pairing.json.tmp." + std::to_wstring(pi.dwProcessId);
    const std::wstring mine = dir + L"\\pairing.json.tmp." + std::to_wstring(GetCurrentProcessId());
    for (const std::wstring& p : {dir + L"\\pairing.json.tmp", dir + L"\\pairing.json.tmp.4000000000", running, mine}) {
        KV_REQUIRE(writeOwnerOnlyFileAtomically(dir, std::filesystem::path(p).filename().wstring(), "{}"));
    }
    note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(note.find("2 pairing.json temp files") != std::string::npos, note);
    KV_CHECK(gone(dir + L"\\pairing.json.tmp") && gone(dir + L"\\pairing.json.tmp.4000000000"));
    KV_CHECK_MSG(!gone(running), "a running process's temp file was removed");
    KV_CHECK_MSG(!gone(mine), "this process's temp file was removed");
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); // its pid no longer names a process
    note = recoverPairingLeftovers(dir);
    KV_CHECK_MSG(gone(running), "a gone process's temp file stayed: " + note);
    KV_CHECK(!gone(mine));
    DeleteFileW(mine.c_str());
}

// No shipping source reaches the host key's test switches. They are declared and built only with KV_TESTING
// (host_state.hpp), so a shipping call doesn't compile; and only test_ targets define it. As a second net, no source
// outside windows/tests names a switch, in any folder under windows/ and core/, but host_state.hpp and
// host_state.cpp themselves.
KV_TEST(no_shipping_source_reaches_the_host_key_test_switch) {
    const std::filesystem::path windows = KV_WINDOWS_SOURCE_DIR;
    const std::filesystem::path core = windows.parent_path() / "core";
    const char* const switches[] = {"setHostKeyDpapiErrorsForTest", "setHostKeyBeforePlainSaveForTest",
                                    "setHostKeyReplaceFaultForTest", "g_wrap_error_for_test",
                                    "g_unwrap_error_for_test", "g_before_plain_save_for_test",
                                    "g_replace_fault_call_for_test", "g_replace_fault_error_for_test"};
    size_t files = 0;
    for (const auto& root : {windows, core}) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory() && it->path() == windows / "tests") {
                it.disable_recursion_pending();
                continue;
            }
            const std::string ext = it->path().extension().string();
            if (!it->is_regular_file() || (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".c" && ext != ".inc"))
                continue;
            ++files;
            const std::string name = it->path().filename().string();
            if (name == "host_state.hpp" || name == "host_state.cpp") continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            for (const char* s : switches) {
                KV_CHECK_MSG(text.str().find(s) == std::string::npos,
                             it->path().lexically_relative(windows.parent_path()).string() + " names " + s);
            }
        }
        KV_CHECK_MSG(!ec, root.string() + ": " + ec.message());
    }
    KV_CHECK_MSG(files > 60, std::to_string(files) + " source files read"); // the tree was found
    // KV_TESTING: defined only for test_ targets in windows/CMakeLists.txt, and nowhere else in a CMake file.
    std::ifstream cm(windows / "CMakeLists.txt", std::ios::binary);
    std::string line;
    size_t defines = 0;
    while (std::getline(cm, line)) {
        if (line.find("KV_TESTING") == std::string::npos || line.find_first_not_of(' ') == line.find('#')) continue;
        ++defines;
        KV_CHECK_MSG(line.rfind("target_compile_definitions(test_", 0) == 0 &&
                         line.find(" PRIVATE KV_TESTING)") != std::string::npos,
                     "windows/CMakeLists.txt defines KV_TESTING outside a test_ target: " + line);
    }
    KV_CHECK_MSG(defines >= 1, "no test_ target defines KV_TESTING");
    std::ifstream top(windows.parent_path() / "CMakeLists.txt", std::ios::binary);
    std::ostringstream topText;
    topText << top.rdbuf();
    KV_CHECK_MSG(topText.str().find("KV_TESTING") == std::string::npos, "the top CMakeLists.txt names KV_TESTING");
}

// outsideCandidates: an empty list is written as [] (the device clears its list),
// an absent one stays absent (an older host), and a bad element fails the whole message.
KV_TEST(v3_outside_candidates_empty_absent_and_bad) {
    PetCameraStateMessage m;
    m.deviceId = "fedcba9876543210";
    m.outsideCandidates = std::vector<PetEndpoint>{};
    const std::string j = toJson(m);
    KV_CHECK_MSG(j.find("\"outsideCandidates\":[]") != std::string::npos, j);
    PetCameraStateMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.outsideCandidates && back.outsideCandidates->empty());
    m.outsideCandidates.reset();
    const std::string none = toJson(m);
    KV_CHECK(none.find("outsideCandidates") == std::string::npos);
    KV_REQUIRE(fromJson(none, back));
    KV_CHECK(!back.outsideCandidates);
    std::string bad = j;
    bad.replace(bad.find("[]"), 2, "[{\"host\":\"100.100.1.2\"}]");
    KV_CHECK(!fromJson(bad, back));
}

KV_TEST_MAIN()
