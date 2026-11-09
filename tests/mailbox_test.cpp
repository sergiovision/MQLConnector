#include "core/mailbox.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace mqlc::core {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(Mailbox, FifoOrder) {
    Mailbox box;
    box.push("a");
    box.push("b");
    EXPECT_EQ(box.try_pop(), "a");
    EXPECT_EQ(box.try_pop(), "b");
    EXPECT_FALSE(box.try_pop());
}

TEST(Mailbox, BoundedDropsOldest) {
    Mailbox box(2);
    EXPECT_TRUE(box.push("1"));
    EXPECT_TRUE(box.push("2"));
    EXPECT_FALSE(box.push("3"));
    EXPECT_EQ(box.size(), 2u);
    EXPECT_EQ(box.try_pop(), "2");
    EXPECT_EQ(box.try_pop(), "3");
}

TEST(Mailbox, WaitTimesOutAccurately) {
    Mailbox box;
    const auto start = Clock::now();
    EXPECT_FALSE(box.wait_pop(100ms));
    const auto elapsed = Clock::now() - start;
    EXPECT_GE(elapsed, 100ms);
    EXPECT_LT(elapsed, 100ms + 50ms);
}

TEST(Mailbox, WaitWakesOnPush) {
    Mailbox box;
    std::jthread producer([&] {
        std::this_thread::sleep_for(20ms);
        box.push("reply");
    });
    const auto start = Clock::now();
    EXPECT_EQ(box.wait_pop(2000ms), "reply");
    EXPECT_LT(Clock::now() - start, 500ms);
}

TEST(Mailbox, CloseWakesWaiter) {
    Mailbox box;
    std::jthread closer([&] {
        std::this_thread::sleep_for(20ms);
        box.close();
    });
    const auto start = Clock::now();
    EXPECT_FALSE(box.wait_pop(2000ms));
    EXPECT_LT(Clock::now() - start, 500ms);
}

TEST(MailboxRegistry, RoutesByChart) {
    MailboxRegistry registry;
    registry.subscribe(1);
    registry.subscribe(2);
    EXPECT_EQ(registry.deliver(1, "for-1"), Delivery::delivered);
    EXPECT_EQ(registry.deliver(2, "for-2"), Delivery::delivered);
    EXPECT_EQ(registry.try_pop(2), "for-2");
    EXPECT_EQ(registry.try_pop(1), "for-1");
    EXPECT_FALSE(registry.try_pop(1));
}

TEST(MailboxRegistry, UnsubscribedChartsAreDropped) {
    MailboxRegistry registry;
    EXPECT_EQ(registry.deliver(9, "x"), Delivery::unsubscribed);
    registry.subscribe(9);
    EXPECT_FALSE(registry.try_pop(9));  // nothing was kept from before the subscription
    registry.deliver(9, "y");
    registry.unsubscribe(9);
    EXPECT_FALSE(registry.is_subscribed(9));
    EXPECT_FALSE(registry.try_pop(9));
    EXPECT_FALSE(registry.wait_pop(9, 10ms));
}

TEST(MailboxRegistry, ResubscribeKeepsQueue) {
    MailboxRegistry registry;
    registry.subscribe(3);
    registry.deliver(3, "kept");
    registry.subscribe(3);
    EXPECT_EQ(registry.try_pop(3), "kept");
}

TEST(MailboxRegistry, Overflow) {
    MailboxRegistry registry(1);
    registry.subscribe(4);
    EXPECT_EQ(registry.deliver(4, "old"), Delivery::delivered);
    EXPECT_EQ(registry.deliver(4, "new"), Delivery::overflowed);
    EXPECT_EQ(registry.try_pop(4), "new");
}

TEST(MailboxRegistry, UnsubscribeWakesWaiter) {
    MailboxRegistry registry;
    registry.subscribe(5);
    std::jthread remover([&] {
        std::this_thread::sleep_for(20ms);
        registry.unsubscribe(5);
    });
    const auto start = Clock::now();
    EXPECT_FALSE(registry.wait_pop(5, 2000ms));
    EXPECT_LT(Clock::now() - start, 500ms);
}

}  // namespace
}  // namespace mqlc::core
