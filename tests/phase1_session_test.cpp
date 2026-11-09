// Integration: the Phase 1 features over real TCP against the in-process loopback server.
// Profiles (AC-3), RouteKey (AC-4), correlation (AC-5), framings (AC-6), config handling (AC-7).
#include "connector/session.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mqlc::connector {
namespace {

using namespace mqlc::test;
using Clock = std::chrono::steady_clock;

/// InitLib JSON: the basics plus `extra` members (already JSON, without braces).
std::string init_with(std::uint16_t port, std::int64_t chart, std::string_view extra, int wait_ms = 2000) {
    std::string json = R"({"Host":"127.0.0.1","Port":)" + std::to_string(port) + R"(,"ChartId":)" +
                       std::to_string(chart) + R"(,"WaitTimeout":)" + std::to_string(wait_ms);
    if (!extra.empty()) json.append(",").append(extra);
    return json + "}";
}

/// Answers every request with the request itself.
std::optional<std::string> mirror(std::string_view request) { return std::string(request); }
std::optional<std::string> silent(std::string_view) { return std::nullopt; }

/// Remembers every request it sees, then delegates.
struct Recorder {
    std::mutex mutex;
    std::vector<std::string> frames;
    tools::LoopbackServer::Responder wrap(tools::LoopbackServer::Responder inner) {
        return [this, inner = std::move(inner)](std::string_view request) {
            {
                const std::scoped_lock lock(mutex);
                frames.emplace_back(request);
            }
            return inner(request);
        };
    }
    std::vector<std::string> seen() {
        const std::scoped_lock lock(mutex);
        return frames;
    }
};

std::filesystem::path fresh_log(const char* name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// `"LogPath":"<path>"` with the path JSON-escaped.
std::string log_path_member(const std::filesystem::path& path) {
    std::string member = R"("LogPath":")";
    core::json::append_escaped(member, path.string());
    return member + "\"";
}

// --- AC-3: generic profile -------------------------------------------------------------------

TEST(GenericProfile, InitLibReturnsAtConnectWithoutHandshake) {
    Recorder recorder;
    tools::LoopbackServer server(0, recorder.wrap(silent));
    ASSERT_TRUE(server.listening()) << server.error();
    Session session;
    const auto start = Clock::now();
    EXPECT_EQ(session.init(init_with(server.port(), 1, R"("Profile":"generic","Account":"42")", 5000)), 1);
    EXPECT_LT(Clock::now() - start, 200ms);
    EXPECT_EQ(session.get_levels("EURUSD", 1), "");
    EXPECT_EQ(session.get_signal_cache("key", "EURUSD", 1), "");
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(recorder.seen().empty());  // no 1026, no 1023, no 1033
    session.destroy(1);
}

TEST(GenericProfile, FinCoreProfileStillHandshakesAgainstTheSameServer) {
    Recorder recorder;
    tools::LoopbackServer server(0, recorder.wrap(silent));
    Session session;
    const auto start = Clock::now();
    EXPECT_EQ(session.init(init_with(server.port(), 1, "", 300)), 1);
    EXPECT_GE(Clock::now() - start, 280ms);  // waited for the unanswered handshake
    ASSERT_TRUE(eventually([&] { return !recorder.seen().empty(); }));
    EXPECT_EQ(member(recorder.seen()[0], "Id"), "1026");
    session.destroy(1);
}

TEST(GenericProfile, RoundTripAndBroadcast) {
    tools::LoopbackServer server(0, mirror);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 8, R"("Profile":"generic")")), 1);
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":8,"Hello":"world"})"), R"({"ChartId":8,"Hello":"world"})");
    server.broadcast(R"({"ChartId":8,"Tick":1.25})");
    std::string heard;
    ASSERT_TRUE(eventually([&] { return !(heard = session.listen(8)).empty(); }));
    EXPECT_EQ(heard, R"({"ChartId":8,"Tick":1.25})");
    session.destroy(8);
}

// --- AC-4: RouteKey --------------------------------------------------------------------------

TEST(RouteKey, FramesRouteByTheConfiguredMember) {
    tools::LoopbackServer server(0, mirror);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 5, R"("Profile":"generic","RouteKey":"Client")")), 1);
    ASSERT_EQ(session.init(init_with(server.port(), 0, "")), 1);  // listens on route 0
    ASSERT_EQ(session.init(init_with(server.port(), 9, "")), 1);

    EXPECT_EQ(session.send_and_wait(R"({"Client":5,"ChartId":9,"Data":"x"})"), R"({"Client":5,"ChartId":9,"Data":"x"})");

    server.broadcast(R"({"Client":"5","ChartId":9,"Data":"to client 5"})");
    server.broadcast(R"({"ChartId":9,"Data":"no Client: route 0"})");
    std::string five;
    std::string zero;
    ASSERT_TRUE(eventually([&] { return !(five = session.listen(5)).empty(); }));
    ASSERT_TRUE(eventually([&] { return !(zero = session.listen(0)).empty(); }));
    EXPECT_EQ(member(five, "Data"), "to client 5");
    EXPECT_EQ(member(zero, "Data"), "no Client: route 0");
    EXPECT_EQ(session.listen(9), "");  // ChartId means nothing with RouteKey=Client
    session.destroy(9);
    session.destroy(0);
    session.destroy(5);
}

// --- AC-5: correlation -----------------------------------------------------------------------

/// Before answering a request, broadcasts an unrelated frame B to the same route; then answers
/// with the request itself (so the reply carries the request's ReqId).
struct BroadcastThenReply {
    std::atomic<tools::LoopbackServer*> server{nullptr};
    tools::LoopbackServer::Responder responder() {
        return [this](std::string_view request) -> std::optional<std::string> {
            const auto route = member(request, "ChartId");
            if (auto* s = server.load()) s->broadcast(R"({"ChartId":)" + route + R"(,"Data":"B"})");
            return std::string(request);
        };
    }
};

TEST(Correlation, ReplyMatchesAndBroadcastStaysQueued) {
    BroadcastThenReply behaviour;
    tools::LoopbackServer server(0, behaviour.responder());
    behaviour.server = &server;
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 3, R"("Profile":"generic","CorrelationKey":"ReqId")")), 1);

    EXPECT_EQ(session.send_and_wait(R"({"ChartId":3,"ReqId":7,"Data":"R"})"), R"({"ChartId":3,"ReqId":7,"Data":"R"})");
    EXPECT_EQ(session.listen(3), R"({"ChartId":3,"Data":"B"})");
    EXPECT_EQ(session.listen(3), "");

    // String correlation values work the same way.
    EXPECT_EQ(member(session.send_and_wait(R"({"ChartId":3,"ReqId":"abc"})"), "ReqId"), "abc");
    EXPECT_EQ(session.listen(3), R"({"ChartId":3,"Data":"B"})");
    session.destroy(3);
}

TEST(Correlation, OffMeansTodaysBehaviour) {
    BroadcastThenReply behaviour;
    tools::LoopbackServer server(0, behaviour.responder());
    behaviour.server = &server;
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 3, R"("Profile":"generic")")), 1);
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":3,"ReqId":7})"), R"({"ChartId":3,"Data":"B"})");
    std::string reply;
    ASSERT_TRUE(eventually([&] { return !(reply = session.listen(3)).empty(); }));
    EXPECT_EQ(reply, R"({"ChartId":3,"ReqId":7})");
    session.destroy(3);
}

TEST(Correlation, RequestWithoutTheMemberGetsTheFirstFrame) {
    BroadcastThenReply behaviour;
    tools::LoopbackServer server(0, behaviour.responder());
    behaviour.server = &server;
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 3, R"("Profile":"generic","CorrelationKey":"ReqId")")), 1);
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":3})"), R"({"ChartId":3,"Data":"B"})");
    session.destroy(3);
}

TEST(Correlation, EightThreadsOnOneRouteEachGetTheirOwnReply) {
    BroadcastThenReply behaviour;  // every request also queues a B on the shared route
    tools::LoopbackServer server(0, behaviour.responder());
    behaviour.server = &server;
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 1, R"("Profile":"generic","CorrelationKey":"ReqId")")), 1);
    constexpr int kThreads = 8;
    constexpr int kRequests = 100;
    std::atomic<int> mismatches{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kRequests; ++i) {
                    const std::string req_id = std::to_string(t * 1000 + i);
                    const std::string reply =
                        session.send_and_wait(R"({"ChartId":1,"ReqId":)" + req_id + R"(,"From":)" +
                                              std::to_string(t) + "}");
                    if (member(reply, "ReqId") != req_id || member(reply, "From") != std::to_string(t))
                        ++mismatches;
                }
            });
        }
    }
    EXPECT_EQ(mismatches.load(), 0);
    int broadcasts = 0;
    while (session.listen(1) == R"({"ChartId":1,"Data":"B"})") ++broadcasts;
    EXPECT_EQ(broadcasts, kThreads * kRequests);  // every B still queued, none stolen
    session.destroy(1);
}

TEST(Correlation, DestroyLibEndsACorrelatedWait) {
    tools::LoopbackServer server(0, silent);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 2, R"("Profile":"generic","CorrelationKey":"ReqId")", 10000)), 1);
    std::string reply = "unset";
    Clock::duration waited{};
    std::jthread waiter([&] {
        const auto start = Clock::now();
        reply = session.send_and_wait(R"({"ChartId":2,"ReqId":1})");
        waited = Clock::now() - start;
    });
    std::this_thread::sleep_for(100ms);
    session.destroy(2);  // the last reference
    waiter.join();
    EXPECT_EQ(reply, "");
    EXPECT_LT(waited, 2000ms);
}

TEST(Correlation, LastDestroyLibEndsWaitsOnOtherRoutesToo) {
    tools::LoopbackServer server(0, silent);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 2, R"("Profile":"generic")", 10000)), 1);
    std::string reply = "unset";
    std::jthread waiter([&] { reply = session.send_and_wait(R"({"ChartId":77})"); });
    std::this_thread::sleep_for(100ms);
    const auto start = Clock::now();
    session.destroy(2);
    waiter.join();
    EXPECT_EQ(reply, "");
    EXPECT_LT(Clock::now() - start, 2000ms);
}

// --- AC-6: framings end to end ---------------------------------------------------------------

TEST(Framing, LengthPrefixRoundTripCarriesAnyByte) {
    core::FramingOptions length;
    length.kind = core::FramingKind::length;
    tools::LoopbackServer server(0, mirror, length);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 4, R"("Profile":"generic","Framing":"length")")), 1);
    // A raw 0x1A and a raw newline inside a string: no escaping is needed with a length prefix.
    const std::string request = "{\"ChartId\":4,\"Data\":\"a\x1A" "b\nc\"}";
    EXPECT_EQ(session.send_and_wait(request), request);
    session.post(R"({"ChartId":4,"Data":"posted"})");
    std::string heard;
    ASSERT_TRUE(eventually([&] { return !(heard = session.listen(4)).empty(); }));
    EXPECT_EQ(member(heard, "Data"), "posted");
    session.destroy(4);
}

TEST(Framing, LengthPrefixCorruptionReconnectsAndResumes) {
    core::FramingOptions length;
    length.kind = core::FramingKind::length;
    tools::LoopbackServer server(0, mirror, length);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 4, R"("Profile":"generic","Framing":"length")")), 1);
    ASSERT_TRUE(eventually([&] { return server.accepted() == 1; }));  // the acceptor polls every 50 ms
    for (const std::uint32_t bad : {0u, 3u, 16u * 1024u * 1024u + 5u}) {
        std::string prefix;
        core::LengthPrefixFramer::append_u32_le(prefix, bad);
        server.broadcast_raw(prefix + "garbage");
        const auto connections = server.accepted();
        ASSERT_TRUE(eventually([&] { return server.accepted() == connections + 1 && session.connected(); }, 5000ms))
            << bad;
        EXPECT_EQ(session.send_and_wait(R"({"ChartId":4,"After":)" + std::to_string(bad) + "}"),
                  R"({"ChartId":4,"After":)" + std::to_string(bad) + "}");
    }
    session.destroy(4);
}

TEST(Framing, NewlineDelimiterWithPrettyPrintedJson) {
    core::FramingOptions newline;
    newline.delimiter = '\n';
    tools::LoopbackServer server(0, mirror, newline);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 6, R"("Profile":"generic","Delimiter":10)")), 1);
    const std::string pretty = "{\n  \"ChartId\": 6,\n  \"Data\": \"line1\nline2\",\n  \"N\": [\n    1,\n    2\n  ]\n}";
    const std::string reply = session.send_and_wait(pretty);
    ASSERT_FALSE(reply.empty());
    EXPECT_EQ(reply.find('\n'), std::string::npos);
    EXPECT_EQ(member(reply, "ChartId"), "6");
    EXPECT_EQ(member(reply, "Data"), "line1\nline2");
    EXPECT_EQ(member(reply, "N"), "[     1,     2   ]");

    session.post(pretty);
    std::string heard;
    ASSERT_TRUE(eventually([&] { return !(heard = session.listen(6)).empty(); }));
    EXPECT_EQ(member(heard, "Data"), "line1\nline2");
    session.destroy(6);
}

TEST(Framing, CustomDelimiterWithFinCoreFramesIntact) {
    core::FramingOptions sub;
    sub.delimiter = '\x1E';
    tools::LoopbackServer server(0, fincore_like(), sub);
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 7, R"("Delimiter":30)")), 1);  // fincore profile
    const std::string reply = session.send_and_wait(request_json(1001, 7, "with \x1E inside"));
    EXPECT_EQ(member(reply, "Data"), "with \x1E inside");
    session.destroy(7);
}

// --- AC-7: configuration handling ------------------------------------------------------------

TEST(SessionConfig, FbeProtocolFallsBackToTcpAndSaysSo) {
    tools::LoopbackServer server(0, mirror);
    const auto log = fresh_log("mqlc_phase1_fbe.log");
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 1, R"("Profile":"generic","Protocol":"fbe",)" + log_path_member(log))),
              1);
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":1})"), R"({"ChartId":1})");
    // A later InitLib asking for something else is told what it did not get.
    ASSERT_EQ(session.init(init_with(server.port(), 2, R"("Port":1,"Profile":"fincore","WaitTimeout":2000)")), 1);
    session.logger().flush();
    const std::string text = read_all(log);
    EXPECT_NE(text.find("Protocol \"fbe\" is not supported yet, using tcp"), std::string::npos) << text;
    EXPECT_NE(text.find("ignored: Port, Profile"), std::string::npos) << text;
    EXPECT_NE(text.find("\"profile\":\"generic\""), std::string::npos) << text;
    session.destroy(2);
    session.destroy(1);
}

TEST(SessionConfig, BadValuesAreLoggedAndDefaulted) {
    tools::LoopbackServer server(0, mirror);
    const auto log = fresh_log("mqlc_phase1_bad.log");
    Session session;
    ASSERT_EQ(session.init(init_with(server.port(), 1,
                                     R"("Profile":"generic","Framing":"carrier-pigeon","Delimiter":99,)" +
                                         log_path_member(log))),
              1);
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":1})"), R"({"ChartId":1})");  // default framing works
    session.logger().flush();
    const std::string text = read_all(log);
    EXPECT_NE(text.find("Framing must be delimiter|length"), std::string::npos) << text;
    EXPECT_NE(text.find("Delimiter must be an integer 1..31"), std::string::npos) << text;
    session.destroy(1);
}

TEST(SessionConfig, LogLevelOffWritesNothing) {
    tools::LoopbackServer server(0, mirror);
    const auto log = fresh_log("mqlc_phase1_off.log");
    {
        Session session;
        ASSERT_EQ(session.init(init_with(server.port(), 1,
                                         R"("Profile":"generic","LogLevel":"off",)" + log_path_member(log))),
                  1);
        (void)session.send_and_wait(R"({"ChartId":1,"bad json")");
        session.destroy(1);
        session.logger().flush();
    }
    EXPECT_TRUE(!std::filesystem::exists(log) || std::filesystem::file_size(log) == 0);
}

TEST(SessionConfig, LogLevelErrorKeepsOnlyFailures) {
    tools::LoopbackServer server(0, mirror);
    const auto log = fresh_log("mqlc_phase1_error.log");
    {
        Session session;
        ASSERT_EQ(session.init(init_with(server.port(), 1,
                                         R"("Profile":"generic","LogLevel":"error",)" + log_path_member(log))),
                  1);
        (void)session.send_and_wait("not json");
        session.destroy(1);
        session.logger().flush();
    }
    const std::string text = read_all(log);
    EXPECT_NE(text.find("not a JSON object"), std::string::npos) << text;
    EXPECT_EQ(text.find("MQLConnector.InitLib("), std::string::npos) << text;
}

}  // namespace
}  // namespace mqlc::connector
