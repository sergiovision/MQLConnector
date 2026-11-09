// Integration: a real Session over TCP against the in-process loopback server.
#include "connector/session.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mqlc::connector {
namespace {

using namespace mqlc::test;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kHealth = 1001;          // SIGNAL_CHECK_HEALTH
constexpr std::int64_t kClosePosition = 1018;   // SIGNAL_CLOSE_POSITION
constexpr std::int64_t kBacktestDecision = 1039;

TEST(Session, RoundTrip) {
    tools::LoopbackServer server(0, fincore_like());
    ASSERT_TRUE(server.listening()) << server.error();
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 7)), 1);
    EXPECT_TRUE(session.initialized());
    EXPECT_EQ(session.references(), 1);

    const std::string reply = session.send_and_wait(request_json(kHealth, 7, "ping"));
    EXPECT_EQ(member(reply, "ChartId"), "7");
    EXPECT_EQ(member(reply, "Id"), "1001");
    EXPECT_EQ(member(reply, "Data"), "ping");
    session.destroy(7);
}

TEST(Session, BadInputReturnsEmpty) {
    tools::LoopbackServer server(0, fincore_like());
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 1)), 1);
    EXPECT_EQ(session.send_and_wait(""), "");
    EXPECT_EQ(session.send_and_wait("not json"), "");
    session.post("{broken");
    session.destroy(1);
}

TEST(Session, NotInitialized) {
    Session session;
    EXPECT_EQ(session.get_levels("EURUSD", 1), Session::kNotInitialized);
    EXPECT_EQ(session.get_signal_cache("k", "EURUSD", 1), Session::kNotInitialized);
    EXPECT_EQ(session.send_and_wait(request_json(kHealth, 1)), "");
    EXPECT_EQ(session.listen(1), "");
}

TEST(Session, GetLevelsReturnsData) {
    tools::LoopbackServer server(0, [](std::string_view request) -> std::optional<std::string> {
        const auto header = core::json::read_signal_header(request);
        if (header->id != profiles::fincore::signal::kLevelsForSymbol) return fincore_like()(request);
        const std::string sym = core::json::to_string(header->sym).value_or("");
        std::string reply = R"({"Id":1023,"ChartId":)" + std::to_string(header->chart_id) + ",\"Data\":";
        reply += sym == "OBJ" ? R"({"levels":[1.1,2.2]})" : sym == "NUL" ? "null" : R"("1.1;2.2")";
        return reply + "}";
    });
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 3)), 1);
    EXPECT_EQ(session.get_levels("EURUSD", 3), "1.1;2.2");
    EXPECT_EQ(session.get_levels("OBJ", 3), R"({"levels":[1.1,2.2]})");
    EXPECT_EQ(session.get_levels("NUL", 3), "");
    const std::string cached = session.get_signal_cache("key", "EURUSD", 3);
    EXPECT_EQ(member(cached, "Id"), std::to_string(profiles::fincore::signal::kGetFromCache));
    session.destroy(3);
}

TEST(Session, BroadcastReachesOnlySubscribedChart) {
    tools::LoopbackServer server(0, fincore_like());
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 1)), 1);
    ASSERT_EQ(session.init(init_json(server.port(), 2)), 1);
    ASSERT_TRUE(eventually([&] { return server.clients() == 1; }));

    server.broadcast(request_json(kClosePosition, 2, "for chart 2"));
    server.broadcast(request_json(kClosePosition, 3, "nobody listens"));
    std::string got;
    ASSERT_TRUE(eventually([&] { return !(got = session.listen(2)).empty(); }));
    EXPECT_EQ(member(got, "Data"), "for chart 2");
    EXPECT_EQ(session.listen(1), "");
    EXPECT_EQ(session.listen(3), "");
    EXPECT_EQ(session.listen(2), "");
    session.destroy(1);
    session.destroy(2);
}

TEST(Session, EightChartsOnEightThreadsNeverCrossDeliver) {
    tools::LoopbackServer server(0, fincore_like());
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 100)), 1);
    constexpr int kCharts = 8;
    constexpr int kRequests = 200;
    std::atomic<int> mismatches{0};
    {
        std::vector<std::jthread> threads;
        for (int c = 0; c < kCharts; ++c) {
            threads.emplace_back([&, c] {
                const std::int64_t chart = 100 + c;
                for (int i = 0; i < kRequests; ++i) {
                    const std::string tag = std::to_string(chart) + "/" + std::to_string(i);
                    const std::string reply = session.send_and_wait(request_json(kHealth, chart, tag));
                    if (member(reply, "ChartId") != std::to_string(chart) || member(reply, "Data") != tag)
                        ++mismatches;
                }
            });
        }
    }
    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_EQ(server.requests(), 1u + kCharts * kRequests);  // + the InitLib log-path request
    session.destroy(100);
}

TEST(Session, TimeoutReturnsEmptyWithinBudget) {
    tools::LoopbackServer server(0, fincore_like(kBacktestDecision));
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 5, 300)), 1);
    const auto start = Clock::now();
    EXPECT_EQ(session.send_and_wait(request_json(kBacktestDecision, 5)), "");
    const auto elapsed = Clock::now() - start;
    EXPECT_GE(elapsed, 300ms - 20ms);
    EXPECT_LE(elapsed, 300ms + 20ms);
    session.destroy(5);
}

TEST(Session, ReconnectsAfterServerRestart) {
    auto server = std::make_unique<tools::LoopbackServer>(0, fincore_like());
    const std::uint16_t port = server->port();
    Session session;
    ASSERT_EQ(session.init(init_json(port, 9, 3000)), 1);
    ASSERT_FALSE(session.send_and_wait(request_json(kHealth, 9)).empty());

    server.reset();  // RST every connection, stop listening
    ASSERT_TRUE(eventually([&] { return !session.connected(); }));
    EXPECT_EQ(session.send_and_wait(request_json(kHealth, 9)).size(), 0u);  // nobody to answer

    server = std::make_unique<tools::LoopbackServer>(port, fincore_like());
    ASSERT_TRUE(server->listening()) << server->error();
    const std::string reply = session.send_and_wait(request_json(kHealth, 9, "after restart"));
    EXPECT_EQ(member(reply, "Data"), "after restart");
    session.destroy(9);
}

TEST(Session, InitWhileServerDownThenConnectsInBackground) {
    const std::uint16_t port = free_port();
    Session session;
    const auto start = Clock::now();
    EXPECT_EQ(session.init(init_json(port, 11, 300)), 0);
    EXPECT_LT(Clock::now() - start, 1000ms);
    EXPECT_TRUE(session.initialized());  // a reference is counted even though the connect failed
    EXPECT_EQ(session.references(), 1);

    tools::LoopbackServer server(port, fincore_like());
    ASSERT_TRUE(server.listening()) << server.error();
    ASSERT_TRUE(eventually([&] { return session.connected(); }, 5000ms));
    EXPECT_EQ(session.init(init_json(port, 12)), 1);
    EXPECT_FALSE(session.send_and_wait(request_json(kHealth, 11)).empty());
    session.destroy(11);
    session.destroy(12);
}

TEST(Session, DestroyCountsReferences) {
    tools::LoopbackServer server(0, fincore_like());
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 1)), 1);
    ASSERT_EQ(session.init(init_json(server.port(), 2)), 1);
    EXPECT_EQ(session.references(), 2);

    session.destroy(1);
    EXPECT_TRUE(session.initialized());
    EXPECT_TRUE(session.connected());
    EXPECT_FALSE(session.send_and_wait(request_json(kHealth, 2)).empty());

    const auto start = Clock::now();
    session.destroy(2);
    EXPECT_LT(Clock::now() - start, 2000ms);
    EXPECT_FALSE(session.initialized());
    EXPECT_EQ(session.references(), 0);
    EXPECT_TRUE(eventually([&] { return server.clients() == 0; }));
    EXPECT_EQ(session.get_levels("EURUSD", 2), Session::kNotInitialized);

    // A later InitLib starts over.
    EXPECT_EQ(session.init(init_json(server.port(), 3)), 1);
    session.destroy(3);
}

TEST(Session, PostNeverBlocksWhenServerStopsReading) {
    tools::LoopbackServer server(0, fincore_like());
    Session session;
    ASSERT_EQ(session.init(init_json(server.port(), 4)), 1);
    server.pause_reading(true);

    // 128 MiB: far beyond the socket buffers and the 64 MiB send queue.
    const std::string big = request_json(1013, 4, std::string(64 * 1024, 'x'));  // SIGNAL_POST_LOG
    auto slowest = Clock::duration::zero();
    for (int i = 0; i < 2048; ++i) {
        const auto t0 = Clock::now();
        session.post(big);
        slowest = std::max(slowest, Clock::now() - t0);
    }
    EXPECT_LT(slowest, 50ms);
    server.pause_reading(false);
    session.destroy(4);
}

}  // namespace
}  // namespace mqlc::connector
