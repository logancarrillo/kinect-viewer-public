// What a host thread hands the viewer's UI thread, kept here rather than in a window message. The message carries
// no pointer: one that another program posts to the viewer's window (any program running as this user can) finds
// nothing here, or only what the host really sent. The receiver takes everything waiting on each message, so an item
// whose message couldn't be posted goes with the next one. Bounded: past kMax waiting, the oldest goes.
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace kvwin {

template <typename T, size_t kMax = 256>
class UiMailbox {
public:
    void put(T item) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= kMax) q_.pop_front();
        q_.push_back(std::move(item));
    }
    std::optional<T> take() {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return std::nullopt;
        std::optional<T> item(std::move(q_.front()));
        q_.pop_front();
        return item;
    }

private:
    std::mutex m_;
    std::deque<T> q_;
};

} // namespace kvwin
