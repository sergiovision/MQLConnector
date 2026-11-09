// Per-route inboxes: the reader thread delivers each incoming frame to the mailbox of its route
// (`ChartId` unless RouteKey says otherwise); MQL threads block on (DoSendMessage) or poll
// (DoListenMessage) their own mailbox.
//
// Correlation (opt-in, CorrelationKey): a DoSendMessage whose request carries a correlation value
// waits for the first frame with an equal value and leaves every other frame queued, in order.
// While such a wait is registered (from before the request is sent), frames carrying its value
// are reserved for it: plain pops skip them.
//
// Concurrency: one mutex per mailbox and one for the registry's map, never both at once, and
// none held across a socket call or a frame callback. Every wait ends on a frame, its timeout,
// the mailbox closing, or a stop request on the caller's stop_token.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/codec.h"

namespace mqlc::core {

inline constexpr std::size_t kDefaultMailboxCapacity = 10'000;

class CorrelatedWait;

/// A bounded FIFO of frames. Thread-safe; any number of producers and consumers.
class Mailbox {
public:
    explicit Mailbox(std::size_t capacity = kDefaultMailboxCapacity) noexcept : capacity_(capacity) {}

    /// Appends a frame, dropping the oldest one when full. Returns false if something was dropped.
    /// Overflow ignores correlation: when an unmatched backlog fills the mailbox, a correlated
    /// reply can be the frame dropped. DoListenMessage is the drain.
    bool push(std::string frame, std::optional<CorrValue> corr = std::nullopt);

    /// The oldest frame not reserved by a correlated wait.
    [[nodiscard]] std::optional<std::string> try_pop();

    /// Waits until an unreserved frame is available, the timeout elapses, the mailbox is closed
    /// or `stop` is requested.
    [[nodiscard]] std::optional<std::string> wait_pop(std::chrono::milliseconds timeout,
                                                      const std::stop_token& stop = {});

    /// Waits for the first frame whose correlation value equals `corr`. Registers the wait only
    /// for its own duration; use CorrelatedWait to register before sending the request.
    [[nodiscard]] std::optional<std::string> wait_pop_matching(const CorrValue& corr,
                                                               std::chrono::milliseconds timeout,
                                                               const std::stop_token& stop = {});

    /// Wakes every waiter; later waits return immediately with whatever is still queued.
    void close();

    [[nodiscard]] std::size_t size() const;

private:
    friend class CorrelatedWait;

    struct Entry {
        std::string frame;
        std::optional<CorrValue> corr;  // set only in correlated mode: no allocation otherwise
    };
    using Entries = std::deque<Entry>;

    [[nodiscard]] Entries::iterator first_unreserved();  // mutex_ held
    [[nodiscard]] std::string take(Entries::iterator it);  // mutex_ held
    void reserve(const CorrValue& corr);
    void release(const CorrValue& corr);
    [[nodiscard]] std::optional<std::string> wait_reserved(const CorrValue& corr,
                                                           std::chrono::milliseconds timeout,
                                                           const std::stop_token& stop);

    mutable std::mutex mutex_;
    std::condition_variable_any ready_;  // a frame arrived, a reservation ended, or closed
    Entries entries_;                    // guarded by mutex_
    // Correlation values with a wait in progress (a multiset: two requests may share one).
    // Non-empty only in correlated mode; pushes then notify_all, since waiters wait for
    // different values. Guarded by mutex_.
    std::vector<CorrValue> reserved_;
    std::size_t capacity_;
    bool closed_ = false;  // guarded by mutex_
};

/// Reserves frames carrying `corr` in `box` for its owner from construction to destruction, so a
/// reply arriving before the owner starts waiting cannot be taken by DoListenMessage.
class CorrelatedWait {
public:
    /// `box` may be null (nobody subscribed the route): wait() then returns nullopt at once.
    CorrelatedWait(std::shared_ptr<Mailbox> box, CorrValue corr);
    ~CorrelatedWait();

    CorrelatedWait(const CorrelatedWait&) = delete;
    CorrelatedWait& operator=(const CorrelatedWait&) = delete;

    [[nodiscard]] std::optional<std::string> wait(std::chrono::milliseconds timeout,
                                                  const std::stop_token& stop = {});

private:
    std::shared_ptr<Mailbox> box_;
    CorrValue corr_;
};

enum class Delivery : std::uint8_t { delivered, overflowed, unsubscribed };

/// Mailboxes keyed by route. Frames for routes nobody subscribed are dropped, as the .NET
/// connector did. Subscribing an existing route keeps its queued frames.
class MailboxRegistry {
public:
    explicit MailboxRegistry(std::size_t capacity = kDefaultMailboxCapacity) noexcept
        : capacity_(capacity) {}

    void subscribe(std::int64_t route);
    /// Removes the mailbox (queued frames are lost) and wakes anyone waiting on it.
    void unsubscribe(std::int64_t route);
    [[nodiscard]] bool is_subscribed(std::int64_t route) const;

    /// Copies `frame` into the route's mailbox, if it has one.
    Delivery deliver(std::int64_t route, std::string_view frame, std::optional<CorrValue> corr = std::nullopt);

    [[nodiscard]] std::optional<std::string> try_pop(std::int64_t route);
    [[nodiscard]] std::optional<std::string> wait_pop(std::int64_t route, std::chrono::milliseconds timeout,
                                                      const std::stop_token& stop = {});
    [[nodiscard]] std::optional<std::string> wait_pop_matching(std::int64_t route, const CorrValue& corr,
                                                               std::chrono::milliseconds timeout,
                                                               const std::stop_token& stop = {});

    /// The route's mailbox, or null; the caller's shared_ptr keeps it alive past an unsubscribe.
    [[nodiscard]] std::shared_ptr<Mailbox> find(std::int64_t route) const;

private:
    // The map lock is held only to look a mailbox up; waiting happens on the mailbox itself.
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, std::shared_ptr<Mailbox>> boxes_;  // guarded by mutex_
    std::size_t capacity_;
};

}  // namespace mqlc::core
