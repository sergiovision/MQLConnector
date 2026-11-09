#include "core/mailbox.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

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


CorrValue id(std::int64_t v) { return CorrValue(v); }

TEST(MailboxCorrelation, MatchingTakesItsFrameAndKeepsTheOthersInOrder) {
    Mailbox box;
    box.push("B1");
    box.push("R5", id(5));
    box.push("B2");
    box.push("R7", id(7));
    EXPECT_EQ(box.wait_pop_matching(id(7), 100ms), "R7");
    EXPECT_EQ(box.try_pop(), "B1");
    EXPECT_EQ(box.try_pop(), "R5");
    EXPECT_EQ(box.try_pop(), "B2");
    EXPECT_FALSE(box.try_pop());
}

TEST(MailboxCorrelation, StringAndIntegerValuesNeverMatch) {
    Mailbox box;
    box.push("text", CorrValue(std::string("7")));
    EXPECT_FALSE(box.wait_pop_matching(id(7), 20ms));
    EXPECT_EQ(box.wait_pop_matching(CorrValue(std::string("7")), 20ms), "text");
}

TEST(MailboxCorrelation, WaitsForALaterFrame) {
    Mailbox box;
    std::jthread producer([&] {
        std::this_thread::sleep_for(30ms);
        box.push("broadcast");
        box.push("reply", id(1));
    });
    EXPECT_EQ(box.wait_pop_matching(id(1), 2000ms), "reply");
    EXPECT_EQ(box.try_pop(), "broadcast");
}

TEST(MailboxCorrelation, TimesOutWithoutTakingOthers) {
    Mailbox box;
    box.push("other", id(2));
    const auto start = Clock::now();
    EXPECT_FALSE(box.wait_pop_matching(id(1), 80ms));
    EXPECT_GE(Clock::now() - start, 80ms);
    EXPECT_EQ(box.size(), 1u);
}

TEST(MailboxCorrelation, ReservedFramesAreSkippedByPlainPops) {
    auto box = std::make_shared<Mailbox>();
    CorrelatedWait wait(box, id(9));  // registered before the "request" leaves
    box->push("reply", id(9));
    box->push("broadcast");
    EXPECT_EQ(box->try_pop(), "broadcast");  // the reply is reserved
    EXPECT_FALSE(box->try_pop());
    EXPECT_EQ(wait.wait(10ms), "reply");
}

TEST(MailboxCorrelation, AbandonedReservationFreesTheFrameAndWakesPlainWaiters) {
    auto box = std::make_shared<Mailbox>();
    auto wait = std::make_unique<CorrelatedWait>(box, id(3));
    box->push("late reply", id(3));
    std::optional<std::string> got;
    std::jthread listener([&] { got = box->wait_pop(2000ms); });
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(box->size(), 1u);  // reserved: the plain waiter has not taken it
    wait.reset();                // the request gave up without taking its reply
    listener.join();
    EXPECT_EQ(got, "late reply");
}

TEST(MailboxCorrelation, EightWaitersOnOneMailboxEachGetTheirOwn) {
    auto box = std::make_shared<Mailbox>();
    constexpr int kWaiters = 8;
    std::vector<std::optional<std::string>> got(kWaiters);
    {
        std::vector<std::jthread> waiters;
        for (int i = 0; i < kWaiters; ++i) {
            waiters.emplace_back([&, i] { got[i] = box->wait_pop_matching(id(i), 3000ms); });
        }
        std::this_thread::sleep_for(30ms);
        for (int i = kWaiters - 1; i >= 0; --i) {  // replies in reverse order, with noise between
            box->push("noise");
            box->push("reply " + std::to_string(i), id(i));
        }
    }
    for (int i = 0; i < kWaiters; ++i) EXPECT_EQ(got[i], "reply " + std::to_string(i));
    EXPECT_EQ(box->size(), static_cast<std::size_t>(kWaiters));  // the noise stays queued
}

TEST(MailboxCorrelation, StopTokenEndsTheWait) {
    Mailbox box;
    std::stop_source stop;
    std::jthread stopper([&] {
        std::this_thread::sleep_for(50ms);
        stop.request_stop();
    });
    const auto start = Clock::now();
    EXPECT_FALSE(box.wait_pop_matching(id(1), 5000ms, stop.get_token()));
    EXPECT_FALSE(box.wait_pop(5000ms, stop.get_token()));  // already stopped: returns at once
    EXPECT_LT(Clock::now() - start, 1000ms);
}

TEST(MailboxCorrelation, CloseEndsTheWait) {
    Mailbox box;
    std::jthread closer([&] {
        std::this_thread::sleep_for(50ms);
        box.close();
    });
    const auto start = Clock::now();
    EXPECT_FALSE(box.wait_pop_matching(id(1), 5000ms));
    EXPECT_LT(Clock::now() - start, 1000ms);
}

TEST(MailboxCorrelation, OverflowDropsTheOldestEvenIfItIsAReply) {
    Mailbox box(2);
    box.push("reply", id(1));
    box.push("b1");
    EXPECT_FALSE(box.push("b2"));
    EXPECT_FALSE(box.wait_pop_matching(id(1), 10ms));  // lost: documented limitation
}

TEST(MailboxCorrelation, RegistryRoutesCorrelatedFrames) {
    MailboxRegistry registry;
    registry.subscribe(4);
    EXPECT_EQ(registry.deliver(4, "r", id(1)), Delivery::delivered);
    EXPECT_EQ(registry.deliver(5, "x", id(1)), Delivery::unsubscribed);
    EXPECT_EQ(registry.wait_pop_matching(4, id(1), 10ms), "r");
    EXPECT_FALSE(registry.wait_pop_matching(5, id(1), 10ms));
    EXPECT_EQ(registry.find(5), nullptr);
}

}  // namespace
}  // namespace mqlc::core
