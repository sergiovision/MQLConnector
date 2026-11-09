#include "core/mailbox.h"

#include <utility>

namespace mqlc::core {

bool Mailbox::push(std::string frame) {
    bool kept_all = true;
    {
        const std::scoped_lock lock(mutex_);
        if (capacity_ == 0) return false;
        if (frames_.size() >= capacity_) {
            frames_.pop_front();
            kept_all = false;
        }
        frames_.push_back(std::move(frame));
    }
    ready_.notify_one();
    return kept_all;
}

std::optional<std::string> Mailbox::try_pop() {
    const std::scoped_lock lock(mutex_);
    if (frames_.empty()) return std::nullopt;
    std::string frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

std::optional<std::string> Mailbox::wait_pop(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    if (!ready_.wait_until(lock, deadline, [this] { return !frames_.empty() || closed_; }))
        return std::nullopt;
    if (frames_.empty()) return std::nullopt;  // closed
    std::string frame = std::move(frames_.front());
    frames_.pop_front();
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
    return frames_.size();
}

void MailboxRegistry::subscribe(std::int64_t chart_id) {
    const std::scoped_lock lock(mutex_);
    auto& box = boxes_[chart_id];
    if (!box) box = std::make_shared<Mailbox>(capacity_);
}

void MailboxRegistry::unsubscribe(std::int64_t chart_id) {
    std::shared_ptr<Mailbox> removed;
    {
        const std::scoped_lock lock(mutex_);
        const auto it = boxes_.find(chart_id);
        if (it == boxes_.end()) return;
        removed = std::move(it->second);
        boxes_.erase(it);
    }
    removed->close();
}

bool MailboxRegistry::is_subscribed(std::int64_t chart_id) const { return find(chart_id) != nullptr; }

Delivery MailboxRegistry::deliver(std::int64_t chart_id, std::string_view frame) {
    const auto box = find(chart_id);
    if (!box) return Delivery::unsubscribed;
    return box->push(std::string(frame)) ? Delivery::delivered : Delivery::overflowed;
}

std::optional<std::string> MailboxRegistry::try_pop(std::int64_t chart_id) {
    const auto box = find(chart_id);
    return box ? box->try_pop() : std::nullopt;
}

std::optional<std::string> MailboxRegistry::wait_pop(std::int64_t chart_id,
                                                     std::chrono::milliseconds timeout) {
    const auto box = find(chart_id);
    return box ? box->wait_pop(timeout) : std::nullopt;
}

std::shared_ptr<Mailbox> MailboxRegistry::find(std::int64_t chart_id) const {
    const std::scoped_lock lock(mutex_);
    const auto it = boxes_.find(chart_id);
    return it == boxes_.end() ? nullptr : it->second;
}

}  // namespace mqlc::core
