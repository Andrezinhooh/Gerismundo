#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace nativecall {

// Used at media boundaries: producers never wait for a slower consumer.
// Closing wakes blocked readers and still permits draining queued values.
template<class T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("queue capacity must be positive");
    }
    bool push(T value) {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        if (queue_.size() == capacity_) { queue_.pop_front(); ++dropped_; }
        queue_.push_back(std::move(value));
        changed_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }
    std::size_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> queue_;
    std::size_t dropped_{};
    bool closed_{};
};

} // namespace nativecall
