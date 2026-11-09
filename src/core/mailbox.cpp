#include "core/mailbox.h"

#include <algorithm>
#include <utility>

namespace mqlc::core {

bool Mailbox::push(std::string frame, std::optional<CorrValue> corr) {
    bool kept_all = true;
    bool wake_all = false;
    {
        const std::scoped_lock lock(mutex_);
        if (capacity_ == 0) return false;
        if (entries_.size() >= capacity_) {
            entries_.pop_front();
            kept_all = false;
        }
        entries_.push_back(Entry{std::move(frame), std::move(corr)});
        wake_all = !reserved_.empty();
    }
    // Plain waiters are interchangeable, so one is enough; correlated waiters each wait for their
    // own value, and only the one whose value arrived can take this frame.
    if (wake_all) {
        ready_.notify_all();
    } else {
        ready_.notify_one();
    }
    return kept_all;
}

Mailbox::Entries::iterator Mailbox::first_unreserved() {
    if (reserved_.empty()) return entries_.begin();
    return std::find_if(entries_.begin(), entries_.end(), [this](const Entry& entry) {
        return !entry.corr || std::find(reserved_.begin(), reserved_.end(), *entry.corr) == reserved_.end();
    });
}

std::string Mailbox::take(Entries::iterator it) {
    std::string frame = std::move(it->frame);
    entries_.erase(it);
    return frame;
}

std::optional<std::string> Mailbox::try_pop() {
    const std::scoped_lock lock(mutex_);
    const auto it = first_unreserved();
    if (it == entries_.end()) return std::nullopt;
    return take(it);
}

std::optional<std::string> Mailbox::wait_pop(std::chrono::milliseconds timeout, const std::stop_token& stop) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    auto it = entries_.end();
    const bool ready = ready_.wait_until(lock, stop, deadline, [&] {
        it = first_unreserved();
        return it != entries_.end() || closed_;
    });
    if (!ready || it == entries_.end()) return std::nullopt;  // timeout, stop, or closed
    return take(it);
}

void Mailbox::reserve(const CorrValue& corr) {
    const std::scoped_lock lock(mutex_);
    reserved_.push_back(corr);
}

void Mailbox::release(const CorrValue& corr) {
    bool wake = false;
    {
        const std::scoped_lock lock(mutex_);
        const auto it = std::find(reserved_.begin(), reserved_.end(), corr);
        if (it != reserved_.end()) reserved_.erase(it);
        // Frames this wait reserved but never took are free again for plain waiters.
        wake = !entries_.empty();
    }
    if (wake) ready_.notify_all();
}

std::optional<std::string> Mailbox::wait_reserved(const CorrValue& corr, std::chrono::milliseconds timeout,
                                                  const std::stop_token& stop) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    auto it = entries_.end();
    const bool ready = ready_.wait_until(lock, stop, deadline, [&] {
        it = std::find_if(entries_.begin(), entries_.end(),
                          [&](const Entry& entry) { return entry.corr && *entry.corr == corr; });
        return it != entries_.end() || closed_;
    });
    if (!ready || it == entries_.end()) return std::nullopt;
    return take(it);
}

std::optional<std::string> Mailbox::wait_pop_matching(const CorrValue& corr, std::chrono::milliseconds timeout,
                                                      const std::stop_token& stop) {
    reserve(corr);
    auto frame = wait_reserved(corr, timeout, stop);
    release(corr);
    return frame;
}

void Mailbox::close() {
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
}

std::size_t Mailbox::size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

CorrelatedWait::CorrelatedWait(std::shared_ptr<Mailbox> box, CorrValue corr)
    : box_(std::move(box)), corr_(std::move(corr)) {
    if (box_) box_->reserve(corr_);
}

CorrelatedWait::~CorrelatedWait() {
    if (box_) box_->release(corr_);
}

std::optional<std::string> CorrelatedWait::wait(std::chrono::milliseconds timeout, const std::stop_token& stop) {
    return box_ ? box_->wait_reserved(corr_, timeout, stop) : std::nullopt;
}

void MailboxRegistry::subscribe(std::int64_t route) {
    const std::scoped_lock lock(mutex_);
    auto& box = boxes_[route];
    if (!box) box = std::make_shared<Mailbox>(capacity_);
}

void MailboxRegistry::unsubscribe(std::int64_t route) {
    std::shared_ptr<Mailbox> removed;
    {
        const std::scoped_lock lock(mutex_);
        const auto it = boxes_.find(route);
        if (it == boxes_.end()) return;
        removed = std::move(it->second);
        boxes_.erase(it);
    }
    removed->close();
}

bool MailboxRegistry::is_subscribed(std::int64_t route) const { return find(route) != nullptr; }

Delivery MailboxRegistry::deliver(std::int64_t route, std::string_view frame, std::optional<CorrValue> corr) {
    const auto box = find(route);
    if (!box) return Delivery::unsubscribed;
    return box->push(std::string(frame), std::move(corr)) ? Delivery::delivered : Delivery::overflowed;
}

std::optional<std::string> MailboxRegistry::try_pop(std::int64_t route) {
    const auto box = find(route);
    return box ? box->try_pop() : std::nullopt;
}

std::optional<std::string> MailboxRegistry::wait_pop(std::int64_t route, std::chrono::milliseconds timeout,
                                                     const std::stop_token& stop) {
    const auto box = find(route);
    return box ? box->wait_pop(timeout, stop) : std::nullopt;
}

std::optional<std::string> MailboxRegistry::wait_pop_matching(std::int64_t route, const CorrValue& corr,
                                                              std::chrono::milliseconds timeout,
                                                              const std::stop_token& stop) {
    const auto box = find(route);
    return box ? box->wait_pop_matching(corr, timeout, stop) : std::nullopt;
}

std::shared_ptr<Mailbox> MailboxRegistry::find(std::int64_t route) const {
    const std::scoped_lock lock(mutex_);
    const auto it = boxes_.find(route);
    return it == boxes_.end() ? nullptr : it->second;
}

}  // namespace mqlc::core
