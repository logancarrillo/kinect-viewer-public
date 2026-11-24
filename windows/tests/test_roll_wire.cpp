// The camera-roll messages on the wire (docs/protocol-v3-pet-camera.md §5.1, §5.2, §5.6-5.10,
// §7.1, §9). Golden JSON and bytes for 0x12-0x19, written out by hand from the spec's tables (keys in
// table order, '/' escaped as "\/" as Swift's JSONEncoder does, base64 values computed separately),
// then the checks a device's requests go through. Pure in-memory: no files, no sockets.
#include "kv_test.hpp"

#include "roll_wire.hpp"
#include "wire_protocol.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace kvhost;

namespace {
using Bytes = std::vector<uint8_t>;
std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }

const char* const kClip = "2026-11-18/14-03-22_motion";
const char* const kClipJson = "2026-11-18\\/14-03-22_motion"; // as it appears inside the JSON

const char* const kStorageJson =
    "{\"usedBytes\":150000000000,\"capBytes\":200000000000,\"freeBytes\":512000000000,\"floorBytes\":20000000000,"
    "\"keepDays\":30}";

RollStorage storage() {
    RollStorage s;
    s.usedBytes = 150000000000ULL;
    s.capBytes = 200000000000ULL;
    s.freeBytes = 512000000000ULL;
    s.keepDays = 30;
    return s; // floorBytes keeps its default, 20,000,000,000 (pet-camera.md §6.4)
}

// s with its one occurrence of from replaced (the test fails unless there is exactly one).
std::string replaced(std::string s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    KV_CHECK_MSG(at != std::string::npos && s.find(from, at + 1) == std::string::npos, from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

bool isCode(const char* got, const char* want) {
    return got == want || (got && want && std::strcmp(got, want) == 0);
}
} // namespace

// ---- the type bytes --------------------------------------------------------------------------

KV_TEST(roll_types_have_their_spec_bytes_and_v3_readers_read_them) {
    KV_CHECK(static_cast<uint8_t>(MsgType::RollListReq) == 0x12 && static_cast<uint8_t>(MsgType::RollList) == 0x13 &&
             static_cast<uint8_t>(MsgType::RollThumbReq) == 0x14 && static_cast<uint8_t>(MsgType::RollThumb) == 0x15 &&
             static_cast<uint8_t>(MsgType::PlaybackReq) == 0x16 && static_cast<uint8_t>(MsgType::PlaybackState) == 0x17 &&
             static_cast<uint8_t>(MsgType::PlaybackConfig) == 0x18 &&
             static_cast<uint8_t>(MsgType::PlaybackAccessUnit) == 0x19);
    for (int raw = 0x12; raw <= 0x19; ++raw) {
        const uint8_t b = static_cast<uint8_t>(raw);
        KV_CHECK_MSG(isKnownMsgType(b, 3) && !isKnownMsgType(b, 2) && !isKnownMsgType(b, 1), std::to_string(raw));
    }
    KV_CHECK(!isKnownMsgType(0x1A, 3) && !isKnownMsgType(0x7E, 3)); // 0x1A and up stay free

    // The host reads devices with a v3 skipping reader: a 0x12 is now a frame it can decode. A frame
    // from a v2 session's point of view (its own reader) is skipped whole, never a violation.
    MsgType t;
    uint8_t flags = 0;
    Bytes payload;
    const Bytes f = packFrame(MsgType::RollListReq, 0, std::string("{\"requestId\":1}"));
    FrameReader host(kHostProtocolVersion, true, kMaxClientPayload);
    host.append(f.data(), f.size());
    KV_CHECK(host.next(t, flags, payload) == FrameReader::Result::Frame && t == MsgType::RollListReq && flags == 0 &&
             S(payload) == "{\"requestId\":1}");
    FrameReader v2(2, true);
    v2.append(f.data(), f.size());
    KV_CHECK(v2.next(t, flags, payload) == FrameReader::Result::Unknown && static_cast<uint8_t>(t) == 0x12);
}

// ---- 0x12 rollListReq ------------------------------------------------------------------------

KV_TEST(roll_list_req_json) {
    RollListRequestMessage days;
    days.requestId = 1;
    KV_CHECK(toJson(days) == "{\"requestId\":1}");
    RollListRequestMessage clips;
    clips.requestId = 18446744073709551615ULL;
    clips.day = "2026-11-18";
    clips.cursor = "MjAyNi0xMS0xNw";
    clips.limit = 100;
    const std::string j = toJson(clips);
    KV_CHECK_MSG(j == "{\"requestId\":18446744073709551615,\"day\":\"2026-11-18\",\"cursor\":\"MjAyNi0xMS0xNw\","
                      "\"limit\":100}",
                 j);
    RollListRequestMessage back;
    back.day = "stale";
    KV_REQUIRE(fromJson(toJson(days), back));
    KV_CHECK(back.requestId == 1 && !back.day && !back.cursor && !back.limit);
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.requestId == clips.requestId && back.day == clips.day && back.cursor == clips.cursor && back.limit == 100);
    KV_CHECK(toJson(back) == j);
    // Any key order, null for absent, unknown keys ignored.
    KV_REQUIRE(fromJson(std::string("{\"limit\":null,\"futureKey\":[1],\"day\":null,\"requestId\":2}"), back));
    KV_CHECK(back.requestId == 2 && !back.day && !back.limit);

    // Malformed (§5.10): ignored, not answered.
    RollListRequestMessage x;
    KV_CHECK(!fromJson(std::string("{}"), x));
    KV_CHECK(!fromJson(std::string("{\"day\":\"2026-11-18\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":-1}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":\"1\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"day\":20261118}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"cursor\":[]}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"limit\":\"50\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"limit\":2.5}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"limit\":4294967296}"), x));
    KV_CHECK(!fromJson(std::string("[{\"requestId\":1}]"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1"), x));
    // Well-formed but out of range: decodes, and the check answers it.
    KV_REQUIRE(fromJson(std::string("{\"requestId\":1,\"limit\":0}"), x));
    KV_CHECK(isCode(checkRollListRequest(x), pet_error::kInvalid));
}

KV_TEST(roll_list_req_checks) {
    RollListRequestMessage m;
    m.requestId = 1;
    KV_CHECK(checkRollListRequest(m) == nullptr && rollListLimit(m) == 50);
    m.limit = 1;
    KV_CHECK(checkRollListRequest(m) == nullptr && rollListLimit(m) == 1);
    m.limit = 100;
    KV_CHECK(checkRollListRequest(m) == nullptr && rollListLimit(m) == 100);
    for (int bad : {0, -1, 101, 2147483647}) {
        m.limit = bad;
        KV_CHECK_MSG(isCode(checkRollListRequest(m), pet_error::kInvalid), std::to_string(bad));
    }
    m.limit.reset();

    // A day that isn't a day: not_found (§5.6).
    m.day = "2026-11-18";
    KV_CHECK(checkRollListRequest(m) == nullptr);
    for (const char* bad : {"2026-02-30", "2026-11-18/..", "..", "", "2026-11-1", "2026/11/18", "latest"}) {
        m.day = bad;
        KV_CHECK_MSG(isCode(checkRollListRequest(m), pet_error::kNotFound), bad);
    }
    m.day = "2026-11-18";

    // A cursor that can't be one of ours: invalid (§5.6).
    m.cursor = "MTc5NTAyODYwMjAwMDoyMDI2LTExLTE4LzE0LTAzLTIyX21vdGlvbg";
    KV_CHECK(checkRollListRequest(m) == nullptr);
    for (const char* bad : {"", "..\\..\\x", "a/b", "a+b", "a b", "a"}) {
        m.cursor = bad;
        KV_CHECK_MSG(isCode(checkRollListRequest(m), pet_error::kInvalid), bad);
    }
    m.cursor = std::string(kMaxRollCursorChars + 1, 'A');
    KV_CHECK(isCode(checkRollListRequest(m), pet_error::kInvalid));

    // Several faults: the first in key order (day, cursor, limit) is the answer.
    m.day = "nope";
    m.cursor = "a/b";
    m.limit = 0;
    KV_CHECK(isCode(checkRollListRequest(m), pet_error::kNotFound));
    m.day.reset();
    KV_CHECK(isCode(checkRollListRequest(m), pet_error::kInvalid));
}

// ---- 0x13 rollList -----------------------------------------------------------------------------

KV_TEST(roll_list_days_json) {
    RollListMessage m;
    m.requestId = 5;
    m.days = std::vector<RollDay>{RollDay{"2026-11-18", 12, 2, 123456789012ULL}, RollDay{"2026-11-17", 3, 0, 0}};
    m.nextCursor = "MjAyNi0xMS0xNw";
    m.storage = storage();
    const std::string j = toJson(m);
    const std::string expected = std::string("{\"requestId\":5,\"days\":[{\"day\":\"2026-11-18\",\"clipCount\":12,") +
                                 "\"starredCount\":2,\"bytes\":123456789012},{\"day\":\"2026-11-17\",\"clipCount\":3," +
                                 "\"starredCount\":0,\"bytes\":0}],\"nextCursor\":\"MjAyNi0xMS0xNw\",\"storage\":" +
                                 kStorageJson + "}";
    KV_CHECK_MSG(j == expected, j);

    RollListMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.requestId == 5 && !back.day && !back.clips && back.nextCursor == m.nextCursor);
    KV_REQUIRE(back.days && back.days->size() == 2);
    KV_CHECK((*back.days)[0].day == "2026-11-18" && (*back.days)[0].clipCount == 12 && (*back.days)[0].starredCount == 2 &&
             (*back.days)[0].bytes == 123456789012ULL);
    KV_CHECK(back.storage.usedBytes == 150000000000ULL && back.storage.capBytes == 200000000000ULL &&
             back.storage.freeBytes == 512000000000ULL && back.storage.floorBytes == 20000000000ULL &&
             back.storage.keepDays == 30);
    KV_CHECK(toJson(back) == j);

    // The last page: no nextCursor. An empty roll: an empty list, written as [].
    RollListMessage last;
    last.requestId = 6;
    last.days = std::vector<RollDay>{};
    last.storage = storage();
    const std::string lj = toJson(last);
    KV_CHECK_MSG(lj == std::string("{\"requestId\":6,\"days\":[],\"storage\":") + kStorageJson + "}", lj);
    KV_CHECK(fromJson(lj, back) && back.days && back.days->empty() && !back.nextCursor);

    // The nested objects on their own.
    KV_CHECK(toJson(RollDay{"2026-01-01", 1, 1, 1}) == "{\"day\":\"2026-01-01\",\"clipCount\":1,\"starredCount\":1,\"bytes\":1}");
    KV_CHECK(toJson(storage()) == kStorageJson);
    RollStorage s;
    KV_CHECK(fromJson(std::string(kStorageJson), s) && s.capBytes == 200000000000ULL);
    KV_CHECK(!fromJson(std::string("{\"usedBytes\":1,\"capBytes\":1,\"freeBytes\":1,\"floorBytes\":1}"), s));
}

KV_TEST(roll_list_clips_json) {
    RollClip motion;
    motion.clipId = kClip;
    motion.startUnixMs = 1795028602000ULL;
    motion.startLocal = "2026-11-18T14:03:22-05:00";
    motion.durationSec = 42.5;
    motion.trigger = toWire(ClipTrigger::Motion);
    motion.starred = true;
    motion.width = 1280;
    motion.height = 720;
    motion.bytes = 13281250;
    motion.hasThumbnail = true;
    RollClip manual;
    manual.clipId = "2026-11-18/09-00-00_manual-2";
    manual.startUnixMs = 1795010400000ULL;
    manual.startLocal = "2026-11-18T09:00:00-05:00";
    manual.durationSec = 600;
    manual.trigger = toWire(ClipTrigger::Manual);
    manual.startedBy = "Kitchen iPad";
    manual.starredByMe = true;
    manual.width = 640;
    manual.height = 530;
    RollListMessage m;
    m.requestId = 7;
    m.day = "2026-11-18";
    m.clips = std::vector<RollClip>{motion, manual};
    m.storage = storage();
    const std::string j = toJson(m);
    const std::string expected =
        std::string("{\"requestId\":7,\"day\":\"2026-11-18\",\"clips\":[{\"clipId\":\"") + kClipJson +
        "\",\"startUnixMs\":1795028602000,\"startLocal\":\"2026-11-18T14:03:22-05:00\",\"durationSec\":42.5,"
        "\"trigger\":\"motion\",\"starred\":true,\"starredByMe\":false,\"width\":1280,\"height\":720,\"bytes\":13281250,"
        "\"hasThumbnail\":true},{\"clipId\":\"2026-11-18\\/09-00-00_manual-2\",\"startUnixMs\":1795010400000,"
        "\"startLocal\":\"2026-11-18T09:00:00-05:00\",\"durationSec\":600,\"trigger\":\"manual\","
        "\"startedBy\":\"Kitchen iPad\",\"starred\":false,\"starredByMe\":true,\"width\":640,\"height\":530,\"bytes\":0,"
        "\"hasThumbnail\":false}],\"storage\":" +
        kStorageJson + "}";
    KV_CHECK_MSG(j == expected, j);

    RollListMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.day == std::string("2026-11-18") && !back.days && !back.nextCursor);
    KV_REQUIRE(back.clips && back.clips->size() == 2);
    const RollClip& a = (*back.clips)[0];
    const RollClip& b = (*back.clips)[1];
    KV_CHECK(a.clipId == kClip && a.startUnixMs == 1795028602000ULL && a.startLocal == motion.startLocal &&
             a.durationSec == 42.5 && clipTriggerFromWire(a.trigger) == ClipTrigger::Motion && !a.startedBy && a.starred &&
             !a.starredByMe && a.width == 1280 && a.height == 720 && a.bytes == 13281250 && a.hasThumbnail);
    KV_CHECK(b.clipId == manual.clipId && b.durationSec == 600 && b.startedBy == std::string("Kitchen iPad") &&
             !b.starred && b.starredByMe && !b.hasThumbnail);
    KV_CHECK(toJson(back) == j);

    // Lenient (§1 rule 6): a trigger this build doesn't know still decodes, as Unknown.
    std::string newer = replaced(j, "\"trigger\":\"motion\"", "\"trigger\":\"sound\"");
    newer.insert(newer.size() - 1, ",\"futureKey\":true");
    KV_REQUIRE(fromJson(newer, back));
    KV_CHECK(clipTriggerFromWire((*back.clips)[0].trigger) == ClipTrigger::Unknown);

    // Malformed: one bad clip, a missing storage or a wrong type fails the whole message.
    RollListMessage x;
    std::string no_storage = j.substr(0, j.find(",\"storage\"")) + "}";
    KV_CHECK(!fromJson(no_storage, x));
    std::string storage_null = j.substr(0, j.find(",\"storage\"")) + ",\"storage\":null}";
    KV_CHECK(!fromJson(storage_null, x));
    KV_CHECK(!fromJson(replaced(j, ",\"hasThumbnail\":true", ""), x));
    KV_CHECK(!fromJson(replaced(j, "\"bytes\":13281250", "\"bytes\":-1"), x));
    const std::string clips_object = std::string("{\"requestId\":7,\"clips\":{},\"storage\":") + kStorageJson + "}";
    KV_CHECK(!fromJson(clips_object, x));
    // startedBy is the device name as a string on the wire, not the sidecar's object (pet-camera.md §6.3).
    KV_CHECK(!fromJson(replaced(j, "\"startedBy\":\"Kitchen iPad\"", "\"startedBy\":{\"deviceName\":\"Kitchen iPad\"}"), x));
}

// ---- 0x14 rollThumbReq / 0x15 rollThumb -------------------------------------------------------

KV_TEST(roll_thumb_json) {
    RollThumbRequestMessage req;
    req.requestId = 3;
    req.clipId = kClip;
    const std::string rj = toJson(req);
    KV_CHECK_MSG(rj == std::string("{\"requestId\":3,\"clipId\":\"") + kClipJson + "\"}", rj);
    RollThumbRequestMessage rb;
    KV_REQUIRE(fromJson(rj, rb));
    KV_CHECK(rb.requestId == 3 && rb.clipId == kClip && checkRollThumbRequest(rb) == nullptr);
    // An unescaped '/' reads the same.
    KV_REQUIRE(fromJson(std::string("{\"clipId\":\"2026-11-18/14-03-22_motion\",\"requestId\":4}"), rb));
    KV_CHECK(rb.clipId == kClip && rb.requestId == 4);
    // Both keys are required (§5.8).
    KV_CHECK(!fromJson(std::string("{\"requestId\":3}"), rb));
    KV_CHECK(!fromJson(std::string("{\"clipId\":\"x\"}"), rb));
    KV_CHECK(!fromJson(std::string("{\"requestId\":3,\"clipId\":null}"), rb));
    KV_CHECK(!fromJson(std::string("{\"requestId\":3,\"clipId\":7}"), rb));
    // A well-formed request with an id that isn't one: not_found (§5.8, §7.1).
    KV_REQUIRE(fromJson(std::string("{\"requestId\":3,\"clipId\":\"..\\\\..\\\\Windows\\\\win.ini\"}"), rb));
    KV_CHECK(rb.clipId == "..\\..\\Windows\\win.ini" && isCode(checkRollThumbRequest(rb), pet_error::kNotFound));

    RollThumbMessage thumb;
    thumb.requestId = 3;
    thumb.clipId = kClip;
    thumb.jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
    const std::string tj = toJson(thumb);
    KV_CHECK_MSG(tj == std::string("{\"requestId\":3,\"clipId\":\"") + kClipJson + "\",\"jpeg\":\"\\/9j\\/2Q==\"}", tj);
    RollThumbMessage tb;
    KV_REQUIRE(fromJson(tj, tb));
    KV_CHECK(tb.requestId == 3 && tb.clipId == kClip && tb.jpeg == thumb.jpeg);
    KV_CHECK(toJson(tb) == tj);

    // At most 64 KiB decoded (§5.8): exactly that decodes, one byte more doesn't.
    thumb.jpeg.assign(RollThumbMessage::kMaxJpegBytes, 0xAB);
    KV_CHECK(fromJson(toJson(thumb), tb) && tb.jpeg.size() == RollThumbMessage::kMaxJpegBytes);
    thumb.jpeg.push_back(0xCD);
    KV_CHECK(!fromJson(toJson(thumb), tb));
    // Not base64, or unpadded: malformed.
    KV_CHECK(!fromJson(std::string("{\"requestId\":3,\"clipId\":\"c\",\"jpeg\":\"*\"}"), tb));
    KV_CHECK(!fromJson(std::string("{\"requestId\":3,\"clipId\":\"c\",\"jpeg\":\"\\/9j\\/2Q\"}"), tb));
    KV_CHECK(!fromJson(std::string("{\"requestId\":3,\"clipId\":\"c\"}"), tb));
}

// ---- 0x16 playbackReq ------------------------------------------------------------------------

KV_TEST(playback_req_json) {
    PlaybackRequestMessage start;
    start.requestId = 30;
    start.action = playback_action::kStart;
    start.clipId = kClip;
    start.positionSec = 12.5;
    const std::string sj = toJson(start);
    KV_CHECK_MSG(sj == std::string("{\"requestId\":30,\"action\":\"start\",\"clipId\":\"") + kClipJson +
                           "\",\"positionSec\":12.5}",
                 sj);
    PlaybackRequestMessage seek;
    seek.requestId = 31;
    seek.action = playback_action::kSeek;
    seek.streamId = 4294967295u;
    seek.positionSec = 0;
    const std::string kj = toJson(seek);
    KV_CHECK_MSG(kj == "{\"requestId\":31,\"action\":\"seek\",\"streamId\":4294967295,\"positionSec\":0}", kj);
    PlaybackRequestMessage stop;
    stop.requestId = 32;
    stop.action = playback_action::kStop;
    stop.streamId = 1;
    KV_CHECK(toJson(stop) == "{\"requestId\":32,\"action\":\"stop\",\"streamId\":1}");

    PlaybackRequestMessage back;
    KV_REQUIRE(fromJson(sj, back));
    KV_CHECK(back.requestId == 30 && back.action == "start" && back.clipId == std::string(kClip) && !back.streamId &&
             back.positionSec == 12.5);
    KV_CHECK(toJson(back) == sj);
    KV_REQUIRE(fromJson(kj, back));
    KV_CHECK(back.action == "seek" && !back.clipId && back.streamId == 4294967295u && back.positionSec == 0.0);
    KV_CHECK(toJson(back) == kj);

    // An unknown action decodes, so it can be answered "unsupported" (§1 rule 6).
    KV_REQUIRE(fromJson(std::string("{\"requestId\":33,\"action\":\"rewind\",\"streamId\":1}"), back));
    KV_CHECK(back.action == "rewind" && !playback_action::isKnown(back.action) &&
             isCode(checkPlaybackRequest(back), pet_error::kUnsupported));

    // Malformed: not answered.
    PlaybackRequestMessage x;
    KV_CHECK(!fromJson(std::string("{\"action\":\"stop\",\"streamId\":1}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"streamId\":1}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"action\":\"stop\",\"streamId\":4294967296}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"action\":\"stop\",\"streamId\":-1}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"action\":\"stop\",\"streamId\":\"1\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"action\":\"seek\",\"streamId\":1,\"positionSec\":\"3\"}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":1,\"action\":\"start\",\"clipId\":[]}"), x));
    KV_CHECK(fromJson(std::string("{\"requestId\":1,\"action\":\"start\",\"clipId\":null,\"positionSec\":null}"), x) &&
             !x.clipId && !x.positionSec);
}

KV_TEST(playback_req_checks) {
    PlaybackRequestMessage m;
    m.requestId = 1;
    m.action = playback_action::kStart;
    KV_CHECK(isCode(checkPlaybackRequest(m), pet_error::kInvalid)); // start needs a clip
    m.clipId = "2026-11-18/14-03-22_motion.mp4";
    KV_CHECK(isCode(checkPlaybackRequest(m), pet_error::kNotFound));
    m.clipId = kClip;
    KV_CHECK(checkPlaybackRequest(m) == nullptr); // no streamId and no position needed
    m.positionSec = -4.0;
    KV_CHECK(checkPlaybackRequest(m) == nullptr); // clamped later, not refused

    for (const char* a : {playback_action::kPause, playback_action::kResume, playback_action::kStop}) {
        PlaybackRequestMessage r;
        r.action = a;
        r.clipId = "not a clip"; // ignored by everything but start
        KV_CHECK_MSG(isCode(checkPlaybackRequest(r), pet_error::kInvalid), a);
        r.streamId = 0; // not a stream any session has; the session answers not_found
        KV_CHECK_MSG(checkPlaybackRequest(r) == nullptr, a);
    }
    PlaybackRequestMessage seek;
    seek.action = playback_action::kSeek;
    seek.streamId = 2;
    KV_CHECK(isCode(checkPlaybackRequest(seek), pet_error::kInvalid)); // seek needs a position
    seek.positionSec = 9.0;
    KV_CHECK(checkPlaybackRequest(seek) == nullptr);
    for (const char* bad : {"", "START", "Start", "play"}) {
        PlaybackRequestMessage r;
        r.action = bad;
        r.streamId = 1;
        r.clipId = kClip;
        KV_CHECK_MSG(isCode(checkPlaybackRequest(r), pet_error::kUnsupported), bad);
    }

    // §5.9: clamped to [0, durationSec].
    KV_CHECK(clampPlaybackPosition(-1.0, 10.0) == 0.0);
    KV_CHECK(clampPlaybackPosition(0.0, 10.0) == 0.0);
    KV_CHECK(clampPlaybackPosition(4.25, 10.0) == 4.25);
    KV_CHECK(clampPlaybackPosition(10.0, 10.0) == 10.0);
    KV_CHECK(clampPlaybackPosition(1e300, 10.0) == 10.0);
    KV_CHECK(clampPlaybackPosition(5.0, 0.0) == 0.0);
}

// ---- 0x17 playbackState ----------------------------------------------------------------------

KV_TEST(playback_state_json_and_lenient_state) {
    PlaybackStateMessage answer;
    answer.requestId = 30;
    answer.streamId = 1;
    answer.clipId = kClip;
    answer.state = toWire(PlaybackState::Starting);
    answer.positionSec = 12;
    answer.durationSec = 42.5;
    const std::string aj = toJson(answer);
    KV_CHECK_MSG(aj == std::string("{\"requestId\":30,\"streamId\":1,\"clipId\":\"") + kClipJson +
                           "\",\"state\":\"starting\",\"positionSec\":12,\"durationSec\":42.5}",
                 aj);
    // A host-initiated stop: no requestId, and the reason's sentence (§5.9).
    PlaybackStateMessage stopped;
    stopped.streamId = 4294967295u;
    stopped.clipId = kClip;
    stopped.state = toWire(PlaybackState::Stopped);
    stopped.positionSec = 3.25;
    stopped.durationSec = 42.5;
    stopped.message = text::kPrivacyMode;
    const std::string sj = toJson(stopped);
    KV_CHECK_MSG(sj == std::string("{\"streamId\":4294967295,\"clipId\":\"") + kClipJson +
                           "\",\"state\":\"stopped\",\"positionSec\":3.25,\"durationSec\":42.5,"
                           "\"message\":\"Privacy mode is on. Turn it off on the PC or an owner's device.\"}",
                 sj);

    PlaybackStateMessage back;
    KV_REQUIRE(fromJson(aj, back));
    KV_CHECK(back.requestId == 30ULL && back.streamId == 1 && back.clipId == kClip &&
             playbackStateFromWire(back.state) == PlaybackState::Starting && back.positionSec == 12.0 &&
             back.durationSec == 42.5 && !back.message);
    KV_CHECK(toJson(back) == aj);
    KV_REQUIRE(fromJson(sj, back));
    KV_CHECK(!back.requestId && back.streamId == 4294967295u && back.message == std::string(text::kPrivacyMode) &&
             playbackStateFromWire(back.state) == PlaybackState::Stopped);
    KV_CHECK(toJson(back) == sj);

    // Every state round-trips; anything else is Unknown, never a failure (§1 rule 6).
    const PlaybackState all[] = {PlaybackState::Starting, PlaybackState::Playing, PlaybackState::Paused,
                                 PlaybackState::Ended,    PlaybackState::Stopped, PlaybackState::Error};
    const char* const names[] = {"starting", "playing", "paused", "ended", "stopped", "error"};
    for (size_t i = 0; i < 6; ++i) {
        KV_CHECK_MSG(std::string(toWire(all[i])) == names[i], names[i]);
        KV_CHECK_MSG(playbackStateFromWire(names[i]) == all[i], names[i]);
    }
    for (const char* other : {"buffering", "Playing", "unknown", ""}) {
        KV_CHECK_MSG(playbackStateFromWire(other) == PlaybackState::Unknown, other);
    }
    KV_CHECK(std::string(toWire(PlaybackState::Unknown)) == "unknown");
    const std::string newer = replaced(aj, "\"state\":\"starting\"", "\"state\":\"buffering\"");
    KV_REQUIRE(fromJson(newer, back));
    KV_CHECK(back.state == "buffering" && playbackStateFromWire(back.state) == PlaybackState::Unknown);

    // Malformed.
    PlaybackStateMessage x;
    KV_CHECK(!fromJson(std::string("{\"streamId\":1,\"clipId\":\"c\",\"positionSec\":0,\"durationSec\":0}"), x));
    KV_CHECK(!fromJson(std::string("{\"streamId\":1,\"clipId\":\"c\",\"state\":\"ended\",\"durationSec\":0}"), x));
    KV_CHECK(!fromJson(std::string("{\"clipId\":\"c\",\"state\":\"ended\",\"positionSec\":0,\"durationSec\":0}"), x));
    KV_CHECK(!fromJson(std::string("{\"requestId\":\"1\",\"streamId\":1,\"clipId\":\"c\",\"state\":\"ended\","
                                   "\"positionSec\":0,\"durationSec\":0}"),
                       x));
    KV_CHECK(fromJson(std::string("{\"requestId\":null,\"streamId\":1,\"clipId\":\"c\",\"state\":\"ended\","
                                  "\"positionSec\":0,\"durationSec\":0,\"message\":null}"),
                      x) &&
             !x.requestId && !x.message);
}

// ---- 0x18 playbackConfig ---------------------------------------------------------------------

KV_TEST(playback_config_json_is_the_0x02_json_plus_three_keys) {
    PlaybackConfigMessage m;
    m.config.streamEpoch = 1;
    m.config.width = 1280;
    m.config.height = 720;
    m.config.modeRaw = 3;
    m.config.sbsLayoutRaw = 0;
    m.config.parameterSets = {{0x67, 0x42, 0x00, 0x1F}, {0x68, 0xCE, 0x3C, 0x80}};
    m.streamId = 1;
    m.clipId = kClip;
    m.durationSec = 42.5;
    const std::string j = toJson(m);
    const std::string expected = std::string("{\"streamEpoch\":1,\"codecRaw\":0,\"width\":1280,\"height\":720,") +
                                 "\"modeRaw\":3,\"sbsLayoutRaw\":0,\"nalLengthHeaderBytes\":4,"
                                 "\"parameterSets\":[\"Z0IAHw==\",\"aM48gA==\"],\"streamId\":1,\"clipId\":\"" +
                                 kClipJson + "\",\"durationSec\":42.5}";
    KV_CHECK_MSG(j == expected, j);
    // Byte for byte the 0x02 JSON of the same config, then the three keys (§5.9, D7).
    const std::string live = toJson(m.config);
    KV_CHECK(j.compare(0, live.size() - 1, live, 0, live.size() - 1) == 0 && j[live.size() - 1] == ',');

    PlaybackConfigMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.config.streamEpoch == 1 && back.config.codecRaw == 0 && back.config.width == 1280 &&
             back.config.height == 720 && back.config.modeRaw == 3 && back.config.nalLengthHeaderBytes == 4 &&
             back.config.parameterSets == m.config.parameterSets && back.streamId == 1 && back.clipId == kClip &&
             back.durationSec == 42.5);
    KV_CHECK(toJson(back) == j);
    // A device parses it with its existing 0x02 code, which ignores the extra keys.
    StreamConfigMessage as_live;
    KV_REQUIRE(fromJson(j, as_live));
    KV_CHECK(as_live.streamEpoch == 1 && as_live.width == 1280 && as_live.parameterSets == m.config.parameterSets);

    // A seek's config: a higher epoch on the same stream.
    m.config.streamEpoch = 2;
    KV_CHECK(toJson(m).find("{\"streamEpoch\":2,") == 0);

    // Malformed: any 0x02 key or any of the three missing.
    PlaybackConfigMessage x;
    KV_CHECK(!fromJson(live, x)); // a plain 0x02 isn't a 0x18
    KV_CHECK(!fromJson(replaced(j, "\"parameterSets\":[\"Z0IAHw==\",\"aM48gA==\"],", ""), x));
    const std::string no_duration = j.substr(0, j.find(",\"durationSec\"")) + "}";
    KV_CHECK(!fromJson(no_duration, x));
    KV_CHECK(!fromJson(replaced(j, "\"streamId\":1", "\"streamId\":4294967296"), x));
    KV_CHECK(!fromJson(replaced(j, "\"clipId\"", "\"clip\""), x));
}

// ---- 0x19 playbackAccessUnit -----------------------------------------------------------------

KV_TEST(playback_access_unit_bytes_are_a_stream_id_then_the_0x03_body) {
    PlaybackAccessUnitHeader h;
    h.streamId = 0x0A0B0C0D;
    h.au.streamEpoch = 2;
    h.au.frameSeq = 7;
    h.au.hostCaptureNs = 33333333; // media time: frame 1 at 30 fps
    h.au.hostEncodeDoneNs = 0;
    h.au.flags = 0x01;
    const uint8_t avcc[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    // Big-endian throughout (§5.9 table): stream id, epoch, frameSeq, hostCaptureNs, hostEncodeDoneNs,
    // flags, then the AVCC payload.
    const uint8_t golden[] = {0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFC, 0xA0, 0x55, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    Bytes out;
    KV_REQUIRE(h.encode(avcc, sizeof(avcc), out));
    KV_CHECK(PlaybackAccessUnitHeader::kBinarySize == 33);
    KV_CHECK(out == Bytes(golden, golden + sizeof(golden)));
    // Everything after the stream id is exactly what 0x03 sends for the same header (D7).
    Bytes live;
    h.au.encode(avcc, sizeof(avcc), live);
    KV_CHECK(Bytes(out.begin() + 4, out.end()) == live);

    PlaybackAccessUnitHeader d;
    const uint8_t* p = nullptr;
    size_t n = 0;
    KV_REQUIRE(PlaybackAccessUnitHeader::decode(out.data(), out.size(), d, &p, &n));
    KV_CHECK(d.streamId == h.streamId && d.au.streamEpoch == 2 && d.au.frameSeq == 7 && d.au.hostCaptureNs == 33333333 &&
             d.au.hostEncodeDoneNs == 0 && d.au.isKeyframe() && !d.au.hasClockOverlay());
    KV_CHECK(n == sizeof(avcc) && std::memcmp(p, avcc, n) == 0);
    // A device's second decoder reads the rest with its 0x03 parser.
    VideoAccessUnitHeader as_live;
    KV_REQUIRE(VideoAccessUnitHeader::decode(out.data() + 4, out.size() - 4, as_live, &p, &n));
    KV_CHECK(as_live.frameSeq == 7 && n == sizeof(avcc));

    // n >= 1: a header alone, or less, isn't an access unit.
    KV_CHECK(!PlaybackAccessUnitHeader::decode(out.data(), PlaybackAccessUnitHeader::kBinarySize, d, &p, &n));
    KV_CHECK(!PlaybackAccessUnitHeader::decode(out.data(), 4, d, &p, &n));
    KV_CHECK(!PlaybackAccessUnitHeader::decode(nullptr, 40, d, &p, &n));
    KV_CHECK(PlaybackAccessUnitHeader::decode(out.data(), PlaybackAccessUnitHeader::kBinarySize + 1, d, &p, &n) && n == 1);
    KV_CHECK(!h.encode(avcc, 0, out) && out.empty());
    KV_CHECK(!h.encode(nullptr, 6, out) && out.empty());

    // The whole frame fits the 4 MiB host -> device cap (§1), or isn't encoded at all.
    Bytes big(kWireMaxPayload - PlaybackAccessUnitHeader::kBinarySize, 0x11);
    KV_REQUIRE(h.encode(big.data(), big.size(), out));
    KV_CHECK(out.size() == kWireMaxPayload);
    const Bytes frame = packFrame(MsgType::PlaybackAccessUnit, 0, out);
    FrameReader device(kProtocolV3);
    device.append(frame.data(), frame.size());
    MsgType t;
    uint8_t flags = 0;
    Bytes payload;
    KV_CHECK(device.next(t, flags, payload) == FrameReader::Result::Frame && t == MsgType::PlaybackAccessUnit &&
             payload.size() == kWireMaxPayload);
    big.push_back(0x11);
    KV_CHECK(!h.encode(big.data(), big.size(), out) && out.empty());
}

// ---- 0x0B: the roll keys (§5.1) ----------------------------------------------------------------

KV_TEST(pet_camera_state_roll_keys) {
    // Nothing set: the pre-roll message, byte for byte (no roll-blocked keys; the protocol goldens pin the rest).
    PetCameraStateMessage bare;
    const std::string base = toJson(bare);
    KV_CHECK(base.find("rollBlocked") == std::string::npos);
    KV_CHECK(base.find("\"rollAvailable\":false,\"playbackAvailable\":false,\"rollEnabled\":false,"
                       "\"motionEnabled\":false,\"motionAvailable\":false,\"motionSensitivity\":5,"
                       "\"scheduleEnabled\":false,\"clipRecording\":false,\"clipManualActive\":false,"
                       "\"clipElapsedSec\":0}") != std::string::npos);

    // A roll state with a clip that recording had to stop.
    PetCameraStateMessage m;
    m.rollAvailable = true;
    m.playbackAvailable = true;
    m.rollEnabled = true;
    m.motionEnabled = true;
    m.motionAvailable = true;
    m.motionSensitivity = 10;
    m.clipRecording = false;
    setRollBlocked(m, std::string(roll_blocked::kStorageFull));
    KV_CHECK(m.rollBlockedReason == std::string("storage_full") && m.rollBlockedText == std::string(text::kStorageFull));
    const std::string j = toJson(m);
    KV_CHECK_MSG(j.find("\"rollAvailable\":true,\"playbackAvailable\":true,\"rollEnabled\":true,\"motionEnabled\":true,"
                        "\"motionAvailable\":true,\"motionSensitivity\":10,\"scheduleEnabled\":false,"
                        "\"clipRecording\":false,\"clipManualActive\":false,\"clipElapsedSec\":0,"
                        "\"rollBlockedReason\":\"storage_full\","
                        "\"rollBlockedText\":\"Recording stopped: the camera roll is full of starred clips.\"}") !=
                     std::string::npos,
                 j);
    PetCameraStateMessage back;
    KV_REQUIRE(fromJson(j, back));
    KV_CHECK(back.rollAvailable && back.playbackAvailable && back.motionSensitivity == 10 &&
             back.rollBlockedReason == m.rollBlockedReason && back.rollBlockedText == m.rollBlockedText);

    // Every roll-blocked reason has its sentence; clearing removes both keys.
    for (const char* r : {roll_blocked::kPrivacyMode, roll_blocked::kRollDisabled, roll_blocked::kRollPreparing,
                          roll_blocked::kSensorOff,
                          roll_blocked::kViewFrozen, roll_blocked::kPcReplaying, roll_blocked::kDiskFloor,
                          roll_blocked::kStorageFull}) {
        setRollBlocked(m, std::string(r));
        KV_CHECK_MSG(roll_blocked::isKnown(r) && m.rollBlockedText && *m.rollBlockedText == text::sentenceFor(r), r);
    }
    setRollBlocked(m, std::nullopt);
    KV_CHECK(!m.rollBlockedReason && !m.rollBlockedText && toJson(m).find("rollBlocked") == std::string::npos);
    KV_CHECK(!roll_blocked::isKnown("busy") && !roll_blocked::isKnown(""));
}

// ---- 0x0C: the roll actions (§5.2) -------------------------------------------------------------

KV_TEST(pet_camera_cmd_roll_actions) {
    // Golden JSON for each roll action as a device sends it, decoded and parsed.
    struct Case {
        const char* json;
        RollCommandKind kind;
    };
    const std::string clip = std::string("\"clipId\":\"") + kClipJson + "\"";
    const Case cases[] = {
        {"{\"requestId\":20,\"action\":\"set_roll\",\"enabled\":true}", RollCommandKind::SetRoll},
        {"{\"requestId\":21,\"action\":\"set_motion\",\"enabled\":false,\"sensitivity\":3}", RollCommandKind::SetMotion},
        {"{\"requestId\":22,\"action\":\"start_clip\"}", RollCommandKind::StartClip},
        {"{\"requestId\":23,\"action\":\"stop_clip\"}", RollCommandKind::StopClip},
    };
    for (const auto& c : cases) {
        PetCameraCommandMessage m;
        KV_REQUIRE(fromJson(std::string(c.json), m));
        KV_CHECK_MSG(toJson(m) == c.json, c.json); // the encoder writes the same bytes
        KV_CHECK(isRollAction(m.action) && pet_action::isKnown(m.action));
        RollCommand rc;
        KV_CHECK_MSG(parseRollCommand(m, rc) == nullptr && rc.kind == c.kind, c.json);
    }
    PetCameraCommandMessage star;
    star.requestId = 24;
    star.action = pet_action::kStarClip;
    star.clipId = kClip;
    star.starred = true;
    const std::string sj = toJson(star);
    KV_CHECK_MSG(sj == "{\"requestId\":24,\"action\":\"star_clip\"," + clip + ",\"starred\":true}", sj);
    PetCameraCommandMessage m;
    RollCommand rc;
    KV_REQUIRE(fromJson(sj, m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.kind == RollCommandKind::Star && rc.clipId == kClip);
    star.starred = false;
    KV_REQUIRE(fromJson(toJson(star), m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.kind == RollCommandKind::Unstar && rc.clipId == kClip);
    PetCameraCommandMessage del;
    del.requestId = 25;
    del.action = pet_action::kDeleteClip;
    del.clipId = kClip;
    const std::string dj = toJson(del);
    KV_CHECK_MSG(dj == "{\"requestId\":25,\"action\":\"delete_clip\"," + clip + "}", dj);
    KV_REQUIRE(fromJson(dj, m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.kind == RollCommandKind::Delete && rc.clipId == kClip);

    // The keys each action carries.
    KV_REQUIRE(fromJson(std::string("{\"requestId\":21,\"action\":\"set_motion\",\"sensitivity\":1}"), m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && !rc.enabled && rc.sensitivity == 1);
    KV_REQUIRE(fromJson(std::string("{\"requestId\":21,\"action\":\"set_motion\",\"enabled\":true}"), m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.enabled == true && !rc.sensitivity);
    KV_REQUIRE(fromJson(std::string("{\"requestId\":20,\"action\":\"set_roll\",\"enabled\":false}"), m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.enabled == false);
    // start_clip and stop_clip ignore keys they don't take.
    KV_REQUIRE(fromJson("{\"requestId\":22,\"action\":\"start_clip\"," + clip + ",\"enabled\":false}", m));
    KV_CHECK(parseRollCommand(m, rc) == nullptr && rc.kind == RollCommandKind::StartClip && rc.clipId.empty());

    // Well-formed but unusable: invalid, or not_found for an id that can't be a clip.
    struct Bad {
        const char* json;
        const char* code;
    };
    const Bad bad[] = {
        {"{\"requestId\":1,\"action\":\"set_roll\"}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"set_motion\"}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"set_motion\",\"sensitivity\":0}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"set_motion\",\"enabled\":true,\"sensitivity\":11}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"set_motion\",\"sensitivity\":-2147483648}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"star_clip\",\"starred\":true}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"star_clip\",\"clipId\":\"2026-11-18\\/14-03-22_motion\"}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"delete_clip\"}", pet_error::kInvalid},
        {"{\"requestId\":1,\"action\":\"delete_clip\",\"clipId\":\"..\\/..\\/secrets\"}", pet_error::kNotFound},
        {"{\"requestId\":1,\"action\":\"delete_clip\",\"clipId\":\"2026-11-18\\/14-03-22_motion\\/..\"}",
         pet_error::kNotFound},
        {"{\"requestId\":1,\"action\":\"star_clip\",\"clipId\":\"clip\\/0001\",\"starred\":true}", pet_error::kNotFound},
        {"{\"requestId\":1,\"action\":\"star_clip\",\"clipId\":\"\",\"starred\":false}", pet_error::kNotFound},
    };
    for (const auto& b : bad) {
        KV_REQUIRE(fromJson(std::string(b.json), m));
        KV_CHECK_MSG(isCode(parseRollCommand(m, rc), b.code), b.json);
    }
    // Not a roll action: the caller handles it, and this answers "unsupported" if it's ever asked.
    for (const char* a : {pet_action::kSetSharing, pet_action::kSetPrivacy, pet_action::kRevokeDevice, "format_disk"}) {
        KV_CHECK_MSG(!isRollAction(a), a);
        PetCameraCommandMessage o;
        o.action = a;
        KV_CHECK_MSG(isCode(parseRollCommand(o, rc), pet_error::kUnsupported), a);
    }
}

// ---- §7.1 clip ids, §5.6 days and cursors ------------------------------------------------------

KV_TEST(clip_ids_match_the_spec_pattern_exactly) {
    for (const char* ok : {"2026-11-18/14-03-22_motion", "2026-11-18/14-03-22_manual", "2026-11-18/14-03-22_schedule",
                           "2026-11-18/00-00-00_manual-2", "2026-11-18/23-59-59_motion-999", "2026-11-18/12-00-00_schedule-0",
                           "2024-02-29/12-00-00_manual", "2000-02-29/12-00-00_manual"}) {
        KV_CHECK_MSG(isValidClipId(ok), ok);
    }
    KV_CHECK(clipIdDay("2026-11-18/14-03-22_motion-12") == "2026-11-18");
    for (const char* bad : {"", "2026-11-18", "2026-11-18/", "2026-11-18/14-03-22", "2026-11-18/14-03-22_",
                            "2026-11-18/14-03-22_sound", "2026-11-18/14-03-22_Motion", "2026-11-18/14-03-22_motions",
                            "2026-11-18/14-03-22_motion-", "2026-11-18/14-03-22_motion-1000", "2026-11-18/14-03-22_motion-a",
                            "2026-11-18/14-03-22_motion--1", "2026-11-18/14-03-22_motion-+1", "2026-11-18/14-03-22_motion.mp4",
                            "2026-11-18/14-03-22_motion/..", "2026-11-18\\14-03-22_motion", "2026-11-18//14-03-22_motion",
                            "/2026-11-18/14-03-22_motion", "../2026-11-18/14-03-22_motion", "2026-11-18/../14-03-22_motion",
                            "2026-11-18/14:03:22_motion", "2026-11-18/14-03-22-motion", "2026-11-18 14-03-22_motion",
                            " 2026-11-18/14-03-22_motion", "2026-11-18/14-03-22_motion ", "2026-11-18/24-00-00_manual",
                            "2026-11-18/14-60-00_manual", "2026-11-18/14-03-60_manual", "2026-02-29/14-03-22_manual",
                            "1900-02-29/14-03-22_manual", "2026-13-18/14-03-22_manual", "2026-00-18/14-03-22_manual",
                            "2026-11-00/14-03-22_manual", "2026-11-31/14-03-22_manual", "C:/Windows/system32_manual",
                            "\xEF\xBC\x92" "026-11-18/14-03-22_manual"}) {
        KV_CHECK_MSG(!isValidClipId(bad), bad);
        KV_CHECK_MSG(clipIdDay(bad).empty(), bad);
    }
    // A NUL inside: a path API would stop there, the check doesn't.
    KV_CHECK(!isValidClipId(std::string("2026-11-18/14-03-22_motion\0-2", 29)));
    KV_CHECK(!isValidClipId(std::string("2026-11-18/14-03-22_moti\0n", 26)));
}

KV_TEST(days_are_real_dates) {
    for (const char* ok : {"2026-11-18", "2026-01-01", "2026-12-31", "2024-02-29", "2000-02-29", "2026-04-30"}) {
        KV_CHECK_MSG(isValidRollDay(ok), ok);
    }
    for (const char* bad : {"", "2026-11-1", "2026-11-180", "2026/11/18", "2026-1-118", "26-11-18", "2026-11-18/",
                            "2026-02-29", "2100-02-29", "2026-04-31", "2026-13-01", "2026-00-10", "2026-11-00",
                            "abcd-ef-gh", "2026-11-1a", "-026-11-18", " 2026-11-1", "..\\..\\..\\"}) {
        KV_CHECK_MSG(!isValidRollDay(bad), bad);
    }
    KV_CHECK(!isValidRollDay(std::string("2026-11-1\0", 10)));
}

KV_TEST(cursors_are_base64url_of_a_bounded_length) {
    // What the host builds (§5.6): base64url of "<startUnixMs>:<clipId>" or of "<day>", with or without
    // padding.
    for (const char* ok : {"MjAyNi0xMS0xNw", "MTc5NTAyODYwMjAwMDoyMDI2LTExLTE4LzE0LTAzLTIyX21vdGlvbg",
                           "MTc5NTAyODYwMjAwMDoyMDI2LTExLTE4LzE0LTAzLTIyX21vdGlvbg==", "abc=", "ab==", "ab", "abc",
                           "abcd", "_-_-"}) {
        KV_CHECK_MSG(isWellShapedRollCursor(ok), ok);
    }
    KV_CHECK(isWellShapedRollCursor(std::string(kMaxRollCursorChars, 'A')));
    KV_CHECK(!isWellShapedRollCursor(std::string(kMaxRollCursorChars + 1, 'A')));
    // The longest real cursor fits with room to spare.
    KV_CHECK(std::string("MTg0NDY3NDQwNzM3MDk1NTE2MTU6MjAyNi0xMS0xOC8xNC0wMy0yMl9zY2hlZHVsZS05OTk").size() <
             kMaxRollCursorChars);
    for (const char* bad : {"", "a", "abcde", "=", "==", "a===", "abcd=", "ab=c", "a+b/", "ab c", "ab\n", "..\\..",
                            "2026-11-18/x", "\xC3\xA9t\xC3\xA9"}) {
        KV_CHECK_MSG(!isWellShapedRollCursor(bad), bad);
    }
}

// ---- §9 codes and answers -----------------------------------------------------------------------

KV_TEST(command_errors_carry_the_spec_sentence) {
    const CommandResultMessage p = makeCommandError(9, pet_error::kPrivacyMode);
    KV_CHECK_MSG(toJson(p) == "{\"requestId\":9,\"ok\":false,\"error\":\"privacy_mode\","
                              "\"message\":\"Privacy mode is on. Turn it off on the PC or an owner's device.\"}",
                 toJson(p));
    const CommandResultMessage nf = makeCommandError(10, pet_error::kNotFound);
    KV_CHECK(toJson(nf) == "{\"requestId\":10,\"ok\":false,\"error\":\"not_found\"}");
    // Every code the roll answers with is a §9 code, and the ones with a sentence carry it.
    for (const char* code : {pet_error::kNotFound, pet_error::kInvalid, pet_error::kUnsupported, pet_error::kBusy,
                             pet_error::kTooManyViewers, pet_error::kPrivacyMode, pet_error::kSharingOff,
                             pet_error::kSharingSchedule, pet_error::kPcReplaying, pet_error::kRollDisabled,
                             pet_error::kSensorOff, pet_error::kViewFrozen, pet_error::kDiskFloor, pet_error::kStorageFull,
                             pet_error::kNotAllowedForRole, pet_error::kInternal}) {
        KV_CHECK_MSG(pet_error::isKnown(code), code);
        const CommandResultMessage r = makeCommandError(1, code);
        const char* s = text::sentenceFor(code);
        KV_CHECK_MSG(!r.ok && r.error == std::string(code) && (s ? r.message == std::string(s) : !r.message), code);
    }
    KV_CHECK(std::string(kStartedByThisPc) == "This PC");
    KV_CHECK(playback_action::isKnown("start") && playback_action::isKnown("pause") && playback_action::isKnown("resume") &&
             playback_action::isKnown("seek") && playback_action::isKnown("stop") && !playback_action::isKnown("play"));
}

KV_TEST_MAIN()
