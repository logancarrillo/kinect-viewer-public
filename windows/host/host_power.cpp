#include "host_power.hpp"

#include <utility>

namespace kvhost {

namespace {
std::string orAnotherProcess(const std::string& holder) {
    return text::holderOrAnotherProcess(holder);
}
} // namespace

HostPowerModel::HostPowerModel(SensorBackend& backend, std::function<void(const Snapshot&)> on_changed)
    : backend_(backend), on_changed_(std::move(on_changed)) {}

HostPowerModel::Snapshot HostPowerModel::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return snap_;
}

void HostPowerModel::set(SensorPowerState s, std::optional<std::string> holder, std::optional<std::string> error) {
    Snapshot copy;
    {
        std::lock_guard<std::mutex> lk(m_);
        snap_.state = s;
        snap_.inUseHolder = s == SensorPowerState::InUseElsewhere ? std::move(holder) : std::nullopt;
        snap_.errorMessage = s == SensorPowerState::Error ? std::move(error) : std::nullopt;
        snap_.generation = generation_;
        copy = snap_;
    }
    if (on_changed_) on_changed_(copy);
}

void HostPowerModel::open() {
    set(SensorPowerState::Starting, std::nullopt, std::nullopt);
    report(backend_.open());
}

void HostPowerModel::reportRestart(const SensorBackend::OpenResult& r) {
    report(r);
}

void HostPowerModel::report(const SensorBackend::OpenResult& r) {
    if (r.ok) {
        set(SensorPowerState::On, std::nullopt, std::nullopt);
        return;
    }
    if (r.lock_blocked) {
        // Taken between the check and the open: name whoever holds it now.
        const auto holder = backend_.externalLockHolder();
        set(SensorPowerState::InUseElsewhere, orAnotherProcess(holder ? *holder : r.error), std::nullopt);
        return;
    }
    set(SensorPowerState::Error, std::nullopt,
        r.error.empty() ? std::string("Failed to start the Kinect source (no fallback performed).") : r.error);
}

void HostPowerModel::request(bool turn_on) {
    ++generation_; // anything pending (a resume) is now stale
    if (on_request_) on_request_(turn_on); // before anything is closed or opened
    resume_wanted_ = false;
    if (!turn_on) {
        if (backend_.isOpen()) backend_.close();
        set(SensorPowerState::Off, std::nullopt, std::nullopt);
        return;
    }
    // Turning On never starts a recording on its own; the backend's open() doesn't.
    if (backend_.isOpen() && snapshot().state == SensorPowerState::On) {
        set(SensorPowerState::On, std::nullopt, std::nullopt); // confirm, change nothing
        return;
    }
    if (const auto holder = backend_.externalLockHolder()) {
        set(SensorPowerState::InUseElsewhere, orAnotherProcess(*holder), std::nullopt);
        return;
    }
    open();
}

uint64_t HostPowerModel::suspend() {
    ++generation_;
    const SensorPowerState before = snapshot().state;
    const bool was_open = backend_.isOpen();
    resume_wanted_ = was_open || before == SensorPowerState::Starting;
    if (was_open) backend_.close();
    if (resume_wanted_ || before != SensorPowerState::Off) set(SensorPowerState::Off, std::nullopt, std::nullopt);
    return generation_;
}

bool HostPowerModel::resumeIfCurrent(uint64_t generation, bool usb_present, const std::string& usb_missing_message) {
    // An Off or On that arrived during the settle delay bumped the generation: it wins.
    if (generation != generation_ || !resume_wanted_) return false;
    resume_wanted_ = false;
    if (!usb_present) {
        set(SensorPowerState::Error, std::nullopt, usb_missing_message);
        return true;
    }
    if (const auto holder = backend_.externalLockHolder()) {
        set(SensorPowerState::InUseElsewhere, orAnotherProcess(*holder), std::nullopt);
        return true;
    }
    open();
    return true;
}

uint64_t HostPowerModel::sensorLost(const std::string& message) {
    ++generation_;
    resume_wanted_ = true; // a replug reopens it, unless an Off or On comes first
    if (backend_.isOpen()) backend_.close();
    set(SensorPowerState::Error, std::nullopt, message);
    return generation_;
}

} // namespace kvhost
