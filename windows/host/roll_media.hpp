// The camera roll's media layer (pet-camera.md §6.1, §6.3 "Leftovers"; protocol v3 §5.9): the pre-roll
// ring of `medium` access units, the H.264 pass-through MP4 writer, the clip reader that playback
// streams from, and the probe that recovers an interrupted clip.
//
// This is part of the colour exception (pet-camera.md §6.6): the camera roll is the only code that
// writes colour, and it does so only through these roll_* files, into the camera-roll folder.
//
// Times: every time here is in 100 ns units ("hns", Media Foundation's unit). A capture time is the
// host's monotonic capture clock (LiveFrame::capture_ns / 100). A media time counts from a clip's
// first sample, which is 0.
//
// Container (decided in pet-camera.md §6.1): fragmented MP4. The tests in test_roll_media.cpp
// prove that IMFSourceReader reads a finished fMP4 back with every frame and its timestamps, that the
// file's sync-sample flags mark exactly the IDRs, that a seek lands on the IDR at or before the
// position, and that a file never finalized (a crash) is readable up to its last fragment (the sink
// cuts a fragment every 10 samples, a third of a second). Plain MP4 stays selectable, and its tests
// show that the same crash loses the whole clip.
//
// Two MPEG-4 source quirks with fMP4, both handled by the reader rather than trusted: it reports the
// first sample of the second fragment as a clean point (the file says otherwise), so a keyframe here is
// a sample with an IDR slice; and SetCurrentPosition exactly on an IDR's time lands on the IDR before
// it, so seek() reads forward to the right one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kvhost {

constexpr int64_t kRollHnsPerSecond = 10000000;
// pet-camera.md §6.1: the ring holds the last 6 s of `medium` output.
constexpr int64_t kRollRingSpanHns = 6 * kRollHnsPerSecond;
// pet-camera.md §6.2: a clip includes up to 5 s of pre-roll.
constexpr int64_t kRollPreRollMaxHns = 5 * kRollHnsPerSecond;
// A safety cap on the ring's bytes. 6 s at 2.5 Mbps is about 2 MB; 16 MiB leaves room for IDR-heavy
// scenes and a higher bitrate without letting a misbehaving encoder grow the ring without limit.
constexpr size_t kRollRingByteCap = 16u * 1024u * 1024u;

// One encoded access unit as the roll keeps it: Annex-B (start codes), as the encoder produced it,
// before annexBToAvcc (pet-camera.md §6.1).
struct RollAccessUnit {
    std::vector<uint8_t> annexb;
    int64_t capture_hns = 0; // the frame's capture time on the host's monotonic clock
    bool idr = false;        // holds an IDR slice (nal_unit_type 5)
    uint32_t width = 0, height = 0; // the encoded size; a clip has one size (pet-camera.md §6.1)
};

// Builds a RollAccessUnit from the encoder's Annex-B output. idr is read from the NAL units.
RollAccessUnit rollAccessUnitFromAnnexB(const uint8_t* annexb, size_t n, int64_t capture_hns, uint32_t width,
                                        uint32_t height);
// Builds one from a TierAccessUnit's form (AVCC with 4-byte lengths, plus the tier's SPS/PPS), so the recorder
// can tap the `medium` tier without changing live_tiers: Annex-B again, with SPS and PPS before an IDR.
RollAccessUnit rollAccessUnitFromAvcc(const std::vector<uint8_t>& avcc, const std::vector<uint8_t>& sps,
                                      const std::vector<uint8_t>& pps, bool idr, int64_t capture_hns,
                                      uint32_t width, uint32_t height);

// Keeps the latest SPS and PPS seen and puts them in front of every IDR that lacks them, so each IDR
// in the ring and in a clip decodes on its own (pet-camera.md §6.1). An encoder may send its parameter
// sets only once, or only in its output media type (MfH264Encoder::sequenceHeader()).
// Not thread-safe.
class RollParameterSets {
public:
    // Annex-B SPS+PPS, e.g. MfH264Encoder::sequenceHeader(), for an encoder of width x height (0 x 0:
    // any size). Ignored when it holds neither.
    void setSequenceHeader(const uint8_t* annexb, size_t n, uint32_t width = 0, uint32_t height = 0);
    // Records any SPS/PPS in au (for au's size), sets au.idr from its NAL units, and inserts the known
    // SPS/PPS in front of an IDR that has none (after a leading access unit delimiter). Returns false
    // for an IDR that can't be made to decode on its own: no SPS/PPS known yet, or only ones recorded for
    // another size (an encoder reopened at a new size must send or set its new ones).
    bool normalize(RollAccessUnit& au);
    bool known() const { return !sps_.empty() && !pps_.empty(); }
    const std::vector<uint8_t>& sps() const { return sps_; } // raw NAL, no start code
    const std::vector<uint8_t>& pps() const { return pps_; }
    // SPS then PPS, each behind a 4-byte start code (MF_MT_MPEG_SEQUENCE_HEADER's form).
    std::vector<uint8_t> sequenceHeader() const;

private:
    std::vector<uint8_t> sps_, pps_;
    uint32_t width_ = 0, height_ = 0; // the size the SPS/PPS were seen at; 0 x 0 = any
};

// The pre-roll ring (pet-camera.md §6.1). Thread-safe: the encoder's output thread pushes, and the
// thread that starts a clip copies the pre-roll out.
//
// Rules:
// - It always starts at an IDR: AUs that arrive while it is empty and are not IDRs are dropped.
// - Every IDR in it carries SPS/PPS (RollParameterSets).
// - Time bound: after each push, whole GOPs are dropped from the front until the newest capture time
//   minus the oldest is at most span_hns. When the only GOP left is longer than that (no later IDR),
//   the ring is emptied and starts again at the next IDR, so it never holds more than span_hns.
// - Byte bound: the same, while it holds more than byte_cap bytes.
// - A clip has one size: an AU of another size empties the ring first (and is kept only if an IDR).
class RollPreRollRing {
public:
    struct Options {
        int64_t span_hns = kRollRingSpanHns;
        size_t byte_cap = kRollRingByteCap;
    };

    RollPreRollRing();
    explicit RollPreRollRing(const Options& opts);

    void setSequenceHeader(const uint8_t* annexb, size_t n, uint32_t width = 0, uint32_t height = 0);
    // Returns true when the AU was kept.
    bool push(RollAccessUnit au);
    // The pre-roll for a clip starting now: a copy of the newest stretch of the ring that starts at an
    // IDR whose capture time is at least (newest capture time - max_hns). Empty when no IDR is that
    // recent; the clip then starts at the next IDR (the writer skips AUs before it).
    std::vector<RollAccessUnit> preRoll(int64_t max_hns = kRollPreRollMaxHns) const;
    void clear();

    size_t size() const;
    size_t bytes() const;
    std::vector<int64_t> captureTimes() const; // of every AU held, oldest first
    int64_t spanHns() const; // newest capture time - oldest, 0 when empty or one AU
    // The latest SPS/PPS seen, for a writer that starts on a live IDR without them.
    std::vector<uint8_t> sequenceHeader() const;

private:
    void trimLocked();
    Options opts_;
    mutable std::mutex m_;
    std::deque<RollAccessUnit> aus_;
    size_t bytes_ = 0;
    RollParameterSets params_;
};

enum class RollContainer { FragmentedMp4, Mp4 };
// The container decision (see the top of this file).
constexpr RollContainer kRollContainer = RollContainer::FragmentedMp4;

struct RollWriterOptions {
    std::wstring path; // the clip's .mp4; it must not exist yet (a clip is never written over)
    // The host's case: RollStore::reserveClip has already taken the name by creating the .mp4 empty
    // (CREATE_NEW), so the writer opens that file instead of creating one. It still never writes over a
    // clip: an existing file that isn't empty is refused.
    bool reserved_empty_file = false;
    RollContainer container = kRollContainer;
    uint32_t width = 0, height = 0; // the clip's one size
    uint32_t fps = 30;              // MF_MT_FRAME_RATE (nominal); sample times are the real ones
    uint32_t bitrate = 2500000;     // MF_MT_AVG_BITRATE (informational), `medium` per §6.1
    // The clip's start (its first frame, pre-roll included) as Unix ms: the file's creation time.
    // 0 leaves the times as Windows and MF set them.
    int64_t start_unix_ms = 0;
    // Annex-B SPS+PPS to use when the first IDR carries none (MfH264Encoder::sequenceHeader()).
    std::vector<uint8_t> sequence_header;
    // The most append() may queue before the writer stops taking frames (the disk can't keep up).
    size_t max_queued_bytes = 64u * 1024u * 1024u;
    // Test seam: WriteSample is taken as failing once this many samples were written (0: never), as a disk
    // that fails mid-clip would. The sink's writes reach the file in their own time, so "after N samples" alone
    // may come before any fragment is on the disk. With test_fail_after_fragments (not 0) the writer waits at
    // that sample until the file holds that many whole fragments (a moof and the whole mdat after it, read as
    // any reader would), up to 10 s, and only then fails: a failure after fragments reached the disk.
    uint64_t test_fail_write_after = 0;
    uint32_t test_fail_after_fragments = 0;
};

struct RollClipResult {
    bool ok = false;     // a finished, readable clip is on disk
    std::string error;   // why not; or, with ok, what was cut short (a queue overflow)
    uint64_t bytes = 0;  // the file's size
    int64_t duration_hns = 0; // last sample's media time + its duration
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    uint64_t skipped_before_idr = 0; // AUs dropped because the clip had not reached its first IDR
    int64_t first_capture_hns = 0, last_capture_hns = 0;
    bool creation_time_set = false; // mvhd and NTFS creation times were set to start_unix_ms
    double durationSec() const { return static_cast<double>(duration_hns) / kRollHnsPerSecond; }
};

// The pass-through writer (pet-camera.md §6.1): IMFSinkWriter with an MFVideoFormat_H264 input type equal
// to its output type, so nothing is re-encoded.
//
// - MF_MT_MPEG_SEQUENCE_HEADER is the Annex-B SPS+PPS from the clip's first IDR.
// - Each sample's time is its capture time minus the first sample's (so the first is 0), and its
//   duration runs to the next sample's time; the last sample repeats the previous duration. Times are
//   kept strictly increasing. So the writer holds one AU back until the next arrives.
// - IDR samples get MFSampleExtension_CleanPoint = 1, the others 0.
// - AUs before the first IDR (with SPS/PPS known) are skipped and counted.
// - Creation time: Media Foundation has no attribute that sets the MP4 creation time (MF_PD_LAST_MODIFIED_TIME
//   is read-only, on the source side). So the writer sets the file's NTFS creation time to the clip start
//   when it creates the file (an interrupted clip keeps it), and after Finalize writes the clip start into
//   the 'mvhd' box's creation time (and the end into its modification time) in place.
//
// Threading: internally queued. append() never waits on Media Foundation or the disk; it copies the AU
// into a queue that the writer's own thread drains (with COM initialised there), so the recorder may call
// it from the encoder's output thread. finalize() blocks until the queue is written and the file is
// finalized, so it belongs on a thread that may wait (not the encoder output thread). One thread at a
// time calls append; finalize may come from another.
class RollClipWriter {
public:
    RollClipWriter();
    ~RollClipWriter(); // abandon() if neither finalize() nor abandon() ran
    RollClipWriter(const RollClipWriter&) = delete;
    RollClipWriter& operator=(const RollClipWriter&) = delete;

    // Creates the file (failing if it exists; or, with reserved_empty_file, opens the empty reservation and
    // fails if it isn't empty), sets its creation time, and starts the writer thread. The sink writer
    // itself opens on the first IDR, when the sequence header is known.
    bool open(const RollWriterOptions& opts, std::string* err);
    bool isOpen() const;

    // Queues one AU. False, with nothing queued, when: the AU's size is not the clip's (a clip has one
    // size; the caller finalizes and starts a new clip), the writer is not open or already failed, or the
    // queue is over max_queued_bytes (then the writer takes nothing more and finalize() keeps what came
    // before, saying so in RollClipResult::error).
    bool append(const RollAccessUnit& au, std::string* err = nullptr);
    bool append(RollAccessUnit&& au, std::string* err = nullptr);
    // Queues the pre-roll, oldest first.
    bool appendAll(const std::vector<RollAccessUnit>& aus, std::string* err = nullptr);

    // Writes everything queued, finalizes the file and sets its times. A clip with no IDR has nothing
    // to show: its file is removed and ok is false.
    RollClipResult finalize();
    // Stops without IMFSinkWriter::Finalize, as a crash or power loss would: whatever the sink already
    // wrote stays on disk. Used by the tests; the app finalizes (pet-camera.md §6.1 "Finalizing a clip").
    void abandon();

    // Test hook: waits until every queued AU has gone to WriteSample (the held-back one excepted).
    bool waitIdle(int timeout_ms);
    // The AUs handed to WriteSample so far (not counting the held-back one).
    uint64_t samplesWritten() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// One native sample of a clip, ready for 0x19 (protocol v3 §5.9).
struct RollSample {
    std::vector<uint8_t> avcc; // 4-byte big-endian lengths; SPS, PPS and AUD removed (annexBToAvcc's form)
    int64_t pts_hns = 0;       // media time from the clip start
    int64_t duration_hns = 0;
    bool keyframe = false;     // holds an IDR slice (nal_unit_type 5): 0x19's flag bit 0
    bool clean_point = false;  // MFSampleExtension_CleanPoint as the source reader gave it (diagnostic)
};

// Reads a clip for playback (pet-camera.md §6.1 "Playback over the protocol"). IMFSourceReader gives
// the native (compressed) samples; nothing is decoded. Not thread-safe; the calling thread must have
// COM initialised.
class RollClipReader {
public:
    RollClipReader();
    ~RollClipReader();
    RollClipReader(const RollClipReader&) = delete;
    RollClipReader& operator=(const RollClipReader&) = delete;

    bool open(const std::wstring& path, std::string* err);
    void close();
    bool isOpen() const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // MF_PD_DURATION, or, when the container doesn't give one (an fMP4 without its finishing data), the
    // end of the last sample, found by reading the clip through once at open.
    int64_t durationHns() const { return duration_hns_; }
    double durationSec() const { return static_cast<double>(duration_hns_) / kRollHnsPerSecond; }
    bool durationFromScan() const { return duration_scanned_; }
    // 0x18 parameterSets (protocol v3 §5.9, as 0x02): [SPS, PPS] as raw NAL units without start codes,
    // from the native media type's MF_MT_MPEG_SEQUENCE_HEADER (or the first keyframe when it has none).
    // The wire encoder base64s them.
    const std::vector<uint8_t>& sps() const { return sps_; }
    const std::vector<uint8_t>& pps() const { return pps_; }
    std::vector<std::vector<uint8_t>> parameterSets() const { return {sps_, pps_}; }

    // Seeks so that next() returns the keyframe at or before pos_hns (clamped to [0, duration]), then
    // the samples after it. It reads ahead from where Media Foundation lands (at most about two GOPs
    // when it lands where it should) to find that keyframe itself.
    bool seek(int64_t pos_hns, std::string* err);

    enum class Read { Sample, End, Error };
    // The next sample, repacked to AVCC. End after the last one. A truncated file (an interrupted fMP4, which
    // open() found by reading it through) gives End, not Error, once its last whole fragment has been read:
    // a recovered clip plays to its end, and seek and resume still work. An error before that is still Error.
    Read next(RollSample& out, std::string* err);
    // open() read the clip through and it stopped on an error: an interrupted clip (what the probe calls truncated).
    bool truncated() const { return truncated_; }

    struct Impl;

private:
    bool readParams(std::string* err);
    Read readRaw(RollSample& out, std::string* err); // the next sample from the source reader itself
    std::unique_ptr<Impl> impl_;
    uint32_t width_ = 0, height_ = 0;
    int64_t duration_hns_ = 0;
    bool duration_scanned_ = false;
    bool truncated_ = false;  // the scan at open stopped on an error
    int64_t scan_end_ = 0;    // the end of the last whole sample the scan read
    int64_t last_end_ = 0;    // the end of the last sample next() returned
    std::vector<uint8_t> sps_, pps_;
};

// What the leftover pass needs to rebuild a sidecar for an .mp4 that has none (pet-camera.md §6.3
// "Leftovers"). Whether to rebuild or delete is roll_store's decision.
struct RollProbeResult {
    uint32_t width = 0, height = 0;
    int64_t duration_hns = 0; // end of the last readable sample
    uint64_t frames = 0, keyframes = 0;
    bool truncated = false;   // reading stopped on an error (an interrupted clip), not at the end
    // start: the file's NTFS creation time, which the writer set to the clip start when it created the
    // file; end: start + duration. Both Unix ms.
    int64_t start_unix_ms = 0, end_unix_ms = 0;
    uint64_t bytes = 0;
    std::vector<uint8_t> sps, pps;
    double durationSec() const { return static_cast<double>(duration_hns) / kRollHnsPerSecond; }
};

// Reads every sample of the file. True when it opens and at least one keyframe-led sample is readable;
// false (with err) otherwise, e.g. a plain MP4 that was never finalized (no 'moov').
bool probeRollClip(const std::wstring& path, RollProbeResult& out, std::string* err);

// Reads the 'mvhd' creation time of an MP4 as Unix ms (0 when the field is 0). False when there is no
// moov/mvhd to read. Used by the tests and available to roll_store.
bool rollMp4CreationUnixMs(const std::wstring& path, int64_t& unix_ms);

} // namespace kvhost
