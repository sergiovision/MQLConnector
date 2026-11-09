// Per-chart inboxes: the reader thread delivers each incoming frame to the mailbox of its
// `ChartId`; MQL threads block on (DoSendMessage) or poll (DoListenMessage) their own mailbox.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mqlc::core {

inline constexpr std::size_t kDefaultMailboxCapacity = 10'000;

/// A bounded FIFO of frames. Thread-safe; any number of producers and consumers.
class Mailbox {
public:
    explicit Mailbox(std::size_t capacity = kDefaultMailboxCapacity) noexcept : capacity_(capacity) {}

    /// Appends a frame, dropping the oldest one when full. Returns false if something was dropped.
    bool push(std::string frame);

    [[nodiscard]] std::optional<std::string> try_pop();

    /// Waits until a frame is available, the timeout elapses or the mailbox is closed.
    [[nodiscard]] std::optional<std::string> wait_pop(std::chrono::milliseconds timeout);

    /// Wakes every waiter; later waits return immediately with whatever is still queued.
    void close();

    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> frames_;  // guarded by mutex_
    std::size_t capacity_;
    bool closed_ = false;  // guarded by mutex_
};

enum class Delivery : std::uint8_t { delivered, overflowed, unsubscribed };

/// Mailboxes keyed by chart id. Frames for charts nobody subscribed are dropped, as the .NET
/// connector did. Subscribing an existing chart keeps its queued frames.
class MailboxRegistry {
public:
    explicit MailboxRegistry(std::size_t capacity = kDefaultMailboxCapacity) noexcept
        : capacity_(capacity) {}

    void subscribe(std::int64_t chart_id);
    /// Removes the mailbox (queued frames are lost) and wakes anyone waiting on it.
    void unsubscribe(std::int64_t chart_id);
    [[nodiscard]] bool is_subscribed(std::int64_t chart_id) const;

    /// Copies `frame` into the chart's mailbox, if it has one.
    Delivery deliver(std::int64_t chart_id, std::string_view frame);

    [[nodiscard]] std::optional<std::string> try_pop(std::int64_t chart_id);
    [[nodiscard]] std::optional<std::string> wait_pop(std::int64_t chart_id,
                                                      std::chrono::milliseconds timeout);

private:
    [[nodiscard]] std::shared_ptr<Mailbox> find(std::int64_t chart_id) const;

    // The map lock is held only to look a mailbox up; waiting happens on the mailbox itself,
    // which the waiter keeps alive through its shared_ptr even if the chart unsubscribes.
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, std::shared_ptr<Mailbox>> boxes_;  // guarded by mutex_
    std::size_t capacity_;
};

}  // namespace mqlc::core
