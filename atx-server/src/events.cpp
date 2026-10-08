#include <atx/server/core.hpp>

#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace atx::server {
struct EventBus::Impl {
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::deque<Event> ring;
    std::size_t capacity;
    std::uint64_t sequence{};
    std::uint64_t reserved_end{std::numeric_limits<std::uint64_t>::max()};
    Store* sequence_store; // Borrowed; constructor contract requires an enclosing lifetime.
    bool closed{};
    Impl(std::size_t size, Store* store) : capacity(size), sequence_store(store) {
        if (size == 0 || size > 100000) {
            throw Error(ErrorCode::invalid, "event capacity must be between 1 and 100000");
        }
        if (sequence_store != nullptr) {
            const auto base = sequence_store->reserve_event_sequences(4096);
            if (base > std::numeric_limits<std::uint64_t>::max() - 4096) {
                throw Error(ErrorCode::unavailable, "invalid event sequence allocation");
            }
            sequence = base + 1; // Skipped boot marker forces a snapshot after every restart.
            reserved_end = base + 4096;
        }
    }
};
EventBus::EventBus(std::size_t capacity, Store* durable_sequence_store)
    : impl_(std::make_unique<Impl>(capacity, durable_sequence_store)) {}
EventBus::~EventBus() = default;
void EventBus::publish(std::string channel, std::string type, std::string job_id,
                       std::string payload_json) {
    if (payload_json.size() > 8192 || job_id.size() > 128 || type.size() > 128 ||
        (channel != "jobs" && channel != "system")) {
        throw Error(ErrorCode::invalid, "invalid event envelope");
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return;
        if (impl_->sequence == impl_->reserved_end) {
            if (impl_->sequence_store == nullptr) {
                throw Error(ErrorCode::unavailable, "event sequence exhausted");
            }
            const auto base = impl_->sequence_store->reserve_event_sequences(4096);
            if (base < impl_->sequence ||
                base > std::numeric_limits<std::uint64_t>::max() - 4096) {
                throw Error(ErrorCode::unavailable, "invalid event sequence allocation");
            }
            impl_->sequence = base;
            impl_->reserved_end = base + 4096;
        }
        if (impl_->ring.size() >= impl_->capacity) impl_->ring.pop_front();
        impl_->ring.push_back({++impl_->sequence, std::move(channel), std::move(type),
                              std::move(job_id), std::move(payload_json), unix_ms(), false});
    }
    impl_->changed.notify_all();
}
std::vector<Event> EventBus::read(std::uint64_t after, std::string_view channel,
                                 std::string_view job_id) const {
    if (channel != "jobs" && channel != "system" && channel != "all") {
        throw Error(ErrorCode::invalid, "unknown event channel");
    }
    std::lock_guard lock(impl_->mutex);
    std::vector<Event> events;
    if (after > impl_->sequence) {
        events.push_back({impl_->sequence, "system", "gap", "", "{}", unix_ms(), true});
        return events;
    }
    if (impl_->ring.empty() && after < impl_->sequence) {
        events.push_back({impl_->sequence, "system", "gap", "", "{}", unix_ms(), true});
        return events;
    }
    if (!impl_->ring.empty() && after < impl_->ring.front().sequence - 1) {
        events.push_back({impl_->ring.front().sequence - 1, "system", "gap", "", "{}",
                          unix_ms(), true});
    }
    for (const auto& event : impl_->ring) {
        if (event.sequence > after && (channel == "all" || channel == event.channel) &&
            (job_id.empty() || event.job_id == job_id)) events.push_back(event);
    }
    return events;
}
void EventBus::wait(std::uint64_t after, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait_for(lock, timeout, [&] {
        return impl_->closed || impl_->sequence != after;
    });
}
void EventBus::close() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
    }
    impl_->changed.notify_all();
}
bool EventBus::closed() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->closed;
}
std::uint64_t EventBus::sequence() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->sequence;
}
} // namespace atx::server
