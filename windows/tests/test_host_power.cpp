// HostPowerModel (protocol v2 power) on the synthetic source:
// On/Off round trips, a lock held by another process, a recording running when Off comes, quick
// toggles in order, and the resume generation (an Off during the reopen delay wins). Synthetic only:
// no real frames, and the recording goes to a temp folder as 16-bit PNGs of the synthetic source.
#include "kv_test.hpp"

#include "host_power.hpp"
#include "kinect_core_c.h"
#include "recorder.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <timeapi.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using kvhost::HostPowerModel;
using kvhost::SensorBackend;
using kvhost::SensorPowerState;

namespace {

std::wstring tempPath(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-power-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
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

// The synthetic engine as the sensor, with the lock check pointed at a test lock file and an
// optional recorder that close() must finish before it stops the engine (as the viewer does).
class SyntheticBackend final : public SensorBackend {
public:
    // The engine is created with a temp lock path: the synthetic source then takes that lock (the
    // core's contract since kv_engine_start_ex), so On and Off can be checked against a real lock file
    // without touching ~\\KINECT-IN-USE.
    // Each test's path is its own (name and pid); one test writes another process's lock there first.
    explicit SyntheticBackend(std::string lock_path) : lock_path_(std::move(lock_path)) {
        eng_ = kv_engine_create(lock_path_.c_str());
    }
    bool lockOnDisk() const { return GetFileAttributesA(lock_path_.c_str()) != INVALID_FILE_ATTRIBUTES; }
    ~SyntheticBackend() override {
        if (recorder.isRecording()) recorder.stop();
        kv_engine_stop(eng_);
        kv_engine_destroy(eng_);
    }
    std::optional<std::string> externalLockHolder() override {
        char owner[256] = {0};
        if (!kv_is_locked_by_other(lock_path_.c_str(), owner, sizeof(owner))) return std::nullopt;
        return std::string(owner);
    }
    OpenResult open() override {
        ++opens;
        log.push_back("open");
        kv_display_params_t p;
        kv_default_display_params(&p);
        p.mode = KV_MODE_DEPTH;
        char e[256] = {0};
        OpenResult r;
        r.ok = fail_next_open.empty() && kv_engine_start_ex(eng_, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, false, e, sizeof(e));
        r.error = fail_next_open.empty() ? std::string(e) : fail_next_open;
        fail_next_open.clear();
        return r;
    }
    void close() override {
        if (recorder.isRecording()) {
            recorder.stop(); // finishes the files before the sensor goes
            log.push_back("recording-finished");
        }
        kv_engine_stop(eng_);
        log.push_back("close");
    }
    bool isOpen() const override { return kv_engine_is_running(eng_); }

    kv_engine_t* engine() { return eng_; }

    kvrec::Recorder recorder;
    std::vector<std::string> log;
    int opens = 0;
    std::string fail_next_open;

private:
    std::string lock_path_;
    kv_engine_t* eng_ = nullptr;
};

struct Recorded {
    std::mutex m;
    std::vector<HostPowerModel::Snapshot> states;
    void add(const HostPowerModel::Snapshot& s) {
        std::lock_guard<std::mutex> lk(m);
        states.push_back(s);
    }
    std::vector<SensorPowerState> kinds() {
        std::lock_guard<std::mutex> lk(m);
        std::vector<SensorPowerState> k;
        for (const auto& s : states) k.push_back(s.state);
        return k;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m);
        states.clear();
    }
};

using V = std::vector<SensorPowerState>;
constexpr auto kOff = SensorPowerState::Off;
constexpr auto kStarting = SensorPowerState::Starting;
constexpr auto kOn = SensorPowerState::On;
constexpr auto kInUse = SensorPowerState::InUseElsewhere;
constexpr auto kError = SensorPowerState::Error;

// A real second process, for a lock that names a live pid that isn't ours.
struct OtherProcess {
    PROCESS_INFORMATION pi{};
    bool start() {
        STARTUPINFOW si{sizeof(si)};
        wchar_t cmd[] = L"cmd.exe /c ping -n 60 127.0.0.1 >nul";
        return CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    }
    void stop() {
        if (pi.hProcess) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
    }
    ~OtherProcess() { stop(); }
};

} // namespace

KV_TEST(on_off_round_trips_report_starting_then_on_then_off) {
    SyntheticBackend be(narrow(tempPath(L"nolock")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    KV_CHECK(power.snapshot().state == kOff);
    for (int round = 0; round < 3; ++round) {
        rec.clear();
        power.request(true);
        KV_CHECK(rec.kinds() == (V{kStarting, kOn}));
        KV_CHECK(be.isOpen());
        KV_CHECK(be.lockOnDisk()); // On takes the lock
        rec.clear();
        power.request(false);
        KV_CHECK(rec.kinds() == (V{kOff}));
        KV_CHECK(!be.isOpen());
        KV_CHECK(!be.lockOnDisk()); // Off releases it
    }
    KV_CHECK(be.opens == 3);
    // On while already On confirms it and opens nothing; Off while Off repeats Off.
    power.request(true);
    rec.clear();
    power.request(true);
    KV_CHECK(rec.kinds() == (V{kOn}));
    KV_CHECK(be.opens == 4);
    power.request(false);
    rec.clear();
    power.request(false);
    KV_CHECK(rec.kinds() == (V{kOff}));
}

// Each On or Off is told before the sensor is opened or closed, so the keep-running block holds the
// person's choice even if the process dies while the sensor closes.
KV_TEST(an_on_or_off_is_told_before_the_sensor_opens_or_closes) {
    SyntheticBackend be(narrow(tempPath(L"told-first")));
    HostPowerModel power(be, [](const HostPowerModel::Snapshot&) {});
    power.setOnRequest([&be](bool on) { be.log.push_back(on ? "asked-on" : "asked-off"); });
    power.request(true);
    power.request(false);
    const std::vector<std::string> want = {"asked-on", "open", "asked-off", "close"};
    std::string got;
    for (const auto& s : be.log) got += s + " ";
    KV_CHECK_MSG(be.log == want, got);
}
KV_TEST(a_lock_held_by_another_process_keeps_it_off_and_names_the_holder) {
    const std::wstring lock = tempPath(L"lock") + L".lock";
    OtherProcess other;
    KV_REQUIRE(other.start());
    {
        FILE* f = _wfopen(lock.c_str(), L"wb");
        KV_REQUIRE(f != nullptr);
        std::fprintf(f, "agent: power-test-holder pid: %lu\n", static_cast<unsigned long>(other.pi.dwProcessId));
        std::fclose(f);
    }
    SyntheticBackend be(narrow(lock));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    power.request(true);
    const auto snap = power.snapshot();
    KV_CHECK(rec.kinds() == (V{kInUse}));
    KV_CHECK(be.opens == 0); // nothing opened while another holds it
    KV_CHECK(!be.isOpen());
    KV_CHECK_MSG(snap.inUseHolder && snap.inUseHolder->find("power-test-holder") != std::string::npos,
                 snap.inUseHolder.value_or("(none)"));
    KV_CHECK(!snap.errorMessage);

    // The holder exits: its lock names a dead pid, so the next On opens.
    other.stop();
    rec.clear();
    power.request(true);
    KV_CHECK(rec.kinds() == (V{kStarting, kOn}));
    KV_CHECK(!power.snapshot().inUseHolder);
    power.request(false);
    DeleteFileW(lock.c_str());
}

KV_TEST(an_open_that_fails_reports_error_with_its_message) {
    SyntheticBackend be(narrow(tempPath(L"fail")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    be.fail_next_open = "Kinect not found on USB";
    power.request(true);
    KV_CHECK(rec.kinds() == (V{kStarting, kError}));
    KV_CHECK(power.snapshot().errorMessage.value_or("") == "Kinect not found on USB");
    rec.clear();
    power.request(true); // and it can be retried
    KV_CHECK(rec.kinds() == (V{kStarting, kOn}));
    KV_CHECK(!power.snapshot().errorMessage);
    power.request(false);
}

KV_TEST(off_while_recording_finishes_the_recording_before_closing_the_sensor) {
    SyntheticBackend be(narrow(tempPath(L"rec-nolock")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    power.request(true);
    KV_REQUIRE(be.isOpen());
    KV_CHECK(be.lockOnDisk());

    const std::wstring base = tempPath(L"rec");
    kvrec::RecorderOptions o;
    o.base_dir = base;
    o.lossless = kvrec::Lossless16::Png16Sequence;
    std::string err;
    const bool started = be.recorder.start(KV_MODE_DEPTH, 512, 424, o, &err);
    KV_CHECK_MSG(started, err);
    if (!started) return;
    std::vector<uint16_t> raw(512 * 424);
    uint64_t last = 0;
    int pushed = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < until) {
        uint64_t seq = 0, arr = 0;
        uint32_t ticks = 0, w = 0, h = 0;
        if (kv_engine_copy_latest_raw16(be.engine(), true, last, raw.data(), raw.size(), &seq, &arr, &ticks, &w, &h)) {
            last = seq;
            if (be.recorder.pushRaw16(raw.data(), w, h, seq, ticks, arr)) ++pushed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    KV_REQUIRE(pushed > 5);

    rec.clear();
    power.request(false);
    KV_CHECK(rec.kinds() == (V{kOff}));
    KV_CHECK(!be.recorder.isRecording());
    KV_CHECK(!be.isOpen());
    KV_CHECK(!be.lockOnDisk()); // and the lock is gone after an Off during a recording
    KV_CHECK(be.log.size() >= 2 && be.log[be.log.size() - 2] == "recording-finished" && be.log.back() == "close");
    const kvrec::RecorderStatus st = be.recorder.status();
    std::printf("  recording: %d pushed, %llu written, %llu dropped, error \"%s\"\n", pushed,
                static_cast<unsigned long long>(st.frames_written), static_cast<unsigned long long>(st.frames_dropped),
                st.error.c_str());
    // stop() drains the queue: every frame accepted before Off is on disk, none cut off by the close.
    KV_CHECK(st.frames_written == static_cast<uint64_t>(pushed));
    KV_CHECK(st.error.empty());
    // Turning On again never starts a recording on its own.
    power.request(true);
    KV_CHECK(be.isOpen() && !be.recorder.isRecording());
    power.request(false);
    removeTree(base);
}

KV_TEST(quick_toggles_are_handled_in_arrival_order) {
    SyntheticBackend be(narrow(tempPath(L"quick")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    // Two threads (the phone's I/O thread and this PC's button) feed one FIFO; one thread drains it,
    // as the viewer's message queue does.
    std::mutex qm;
    std::deque<bool> q;
    auto post = [&](bool on) {
        std::lock_guard<std::mutex> lk(qm);
        q.push_back(on);
    };
    post(true);
    post(false); // sent right after: the sensor must end Off
    while (true) {
        bool on = false;
        {
            std::lock_guard<std::mutex> lk(qm);
            if (q.empty()) break;
            on = q.front();
            q.pop_front();
        }
        power.request(on);
    }
    KV_CHECK(rec.kinds() == (V{kStarting, kOn, kOff}));
    KV_CHECK(!be.isOpen());
    rec.clear();
    post(false);
    post(true);
    post(false);
    post(true);
    while (true) {
        bool on = false;
        {
            std::lock_guard<std::mutex> lk(qm);
            if (q.empty()) break;
            on = q.front();
            q.pop_front();
        }
        power.request(on);
    }
    KV_CHECK(rec.kinds() == (V{kOff, kStarting, kOn, kOff, kStarting, kOn}));
    KV_CHECK(be.isOpen());
    // The generation went up with every request, so nothing pending from before them survives.
    KV_CHECK(power.snapshot().generation >= 6);
    power.request(false);
}

KV_TEST(an_off_during_the_resume_delay_wins_unlike_the_macs_wake_path) {
    SyntheticBackend be(narrow(tempPath(L"wake")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    power.request(true);
    rec.clear();
    const uint64_t gen = power.suspend();
    KV_CHECK(rec.kinds() == (V{kOff}));
    KV_CHECK(!be.isOpen() && power.resumeWanted());

    power.request(false); // the phone's Off arrives during the settle delay
    rec.clear();
    KV_CHECK(!power.resumeIfCurrent(gen, true, "no usb"));
    KV_CHECK(rec.kinds().empty());
    KV_CHECK(!be.isOpen() && power.snapshot().state == kOff);

    // Without anything in between, the resume reopens.
    power.request(true);
    const uint64_t gen2 = power.suspend();
    rec.clear();
    KV_CHECK(power.resumeIfCurrent(gen2, true, "no usb"));
    KV_CHECK(rec.kinds() == (V{kStarting, kOn}));
    KV_CHECK(be.isOpen());

    // A resume with the Kinect's USB missing reports Error with the message.
    const uint64_t gen3 = power.suspend();
    rec.clear();
    KV_CHECK(power.resumeIfCurrent(gen3, false, "Woke from sleep; waiting for the Kinect's USB."));
    KV_CHECK(rec.kinds() == (V{kError}));
    KV_CHECK(power.snapshot().errorMessage.value_or("") == "Woke from sleep; waiting for the Kinect's USB.");

    // Suspending while Off reports nothing new and resumes nothing.
    power.request(false);
    rec.clear();
    const uint64_t gen4 = power.suspend();
    KV_CHECK(rec.kinds().empty() && !power.resumeWanted());
    KV_CHECK(!power.resumeIfCurrent(gen4, true, ""));
    KV_CHECK(!be.isOpen());
}

KV_TEST(an_unplug_reports_error_and_a_replug_reopens_unless_an_off_came_first) {
    SyntheticBackend be(narrow(tempPath(L"usb")));
    Recorded rec;
    HostPowerModel power(be, [&](const HostPowerModel::Snapshot& s) { rec.add(s); });
    power.request(true);
    rec.clear();
    const uint64_t gen = power.sensorLost("Kinect USB unplugged");
    KV_CHECK(rec.kinds() == (V{kError}));
    KV_CHECK(!be.isOpen());
    rec.clear();
    KV_CHECK(power.resumeIfCurrent(gen, true, ""));
    KV_CHECK(rec.kinds() == (V{kStarting, kOn}));

    const uint64_t gen2 = power.sensorLost("Kinect USB unplugged");
    power.request(false);
    rec.clear();
    KV_CHECK(!power.resumeIfCurrent(gen2, true, ""));
    KV_CHECK(rec.kinds().empty() && !be.isOpen());
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the PNG16 recorder writes through WIC
    return kvtest::runAll(argc, argv);
}
