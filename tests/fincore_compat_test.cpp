// Golden-frame regression tests for FinCore, the connector's first and frozen client.
//
// Written against the 1.0.0 code before the Phase 1 refactor and kept unchanged since: they pin
// the exact bytes the session hands to its transport (the frame payload; the framer only adds the
// 0x1A after it) and the exact strings returned to MQL, for both InitLib shapes FinCore uses
// (MQL5Utils::SetupTcpConnection sends numbers, CBacktestDecisionClient strings), GetLevels,
// GetSignalCache, DoSendMessage, DoPostMessage and DoListenMessage. A fake transport answers
// synchronously, so the tests are deterministic and need no sockets.
#include "connector/session.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mqlc::connector {
namespace {

using namespace std::string_literals;

/// Everything the session sent, and a way to push frames at it as the server would.
struct FakeWire {
    using Responder = std::function<std::vector<std::string>(std::string_view sent)>;

    std::mutex mutex;
    std::vector<std::string> sent;  // payloads exactly as Session passed them to send()
    net::Transport::FrameHandler deliver;
    Responder respond;
    int transports_created = 0;
    std::string host;
    std::uint16_t port = 0;
    long long wait_timeout_ms = 0;

    std::vector<std::string> take() {
        const std::scoped_lock lock(mutex);
        return std::exchange(sent, {});
    }
};

class FakeTransport final : public net::Transport {
public:
    explicit FakeTransport(std::shared_ptr<FakeWire> wire) : wire_(std::move(wire)) {}

    bool connect(std::chrono::milliseconds) override { return true; }
    bool connected() const noexcept override { return true; }
    bool send(std::string_view frame) override {
        std::vector<std::string> replies;
        {
            const std::scoped_lock lock(wire_->mutex);
            wire_->sent.emplace_back(frame);
        }
        if (wire_->respond) replies = wire_->respond(frame);
        for (const auto& reply : replies) wire_->deliver(reply);  // as the reader thread would
        return true;
    }
    void stop() override {}

private:
    std::shared_ptr<FakeWire> wire_;
};

/// A Session wired to a FakeWire. The variadic tail keeps this independent of the factory's
/// later parameters (the logging callback's exact type).
struct Rig {
    std::shared_ptr<FakeWire> wire = std::make_shared<FakeWire>();
    Session session;

    Rig()
        : session([w = wire](const core::Config& config, net::Transport::FrameHandler on_frame,
                             auto&&...) -> std::unique_ptr<net::Transport> {
              {
                  const std::scoped_lock lock(w->mutex);
                  ++w->transports_created;
                  w->host = config.host;
                  w->port = config.port;
                  w->wait_timeout_ms = static_cast<long long>(config.wait_timeout.count());
              }
              w->deliver = std::move(on_frame);
              return std::make_unique<FakeTransport>(w);
          }) {}
};

/// FinCore's answer to the log-path handshake: an empty Data keeps the default log file.
std::vector<std::string> answer_handshake(std::string_view sent) {
    if (sent.find("\"Id\":1026") != std::string_view::npos) {
        const auto chart = sent.substr(sent.find(':') + 1, sent.find(',') - sent.find(':') - 1);
        return {R"({"Id":1026,"ChartId":)" + std::string(chart) + R"(,"Data":""})"};
    }
    return {};
}

// MQL5Utils::SetupTcpConnection (SerializableEntity, numeric values).
constexpr std::string_view kNumericInit =
    R"({"Account":5012345,"ChartId":131,"Host":"127.0.0.1","Port":2022,"Protocol":"tcp",)"
    R"("WaitOp":300,"WaitTimeout":400})";
// CBacktestDecisionClient (CJAVal, every value a string).
constexpr std::string_view kStringInit =
    R"({"Account":"5012345","ChartId":"133000000000000001","Host":"10.0.0.5","Port":"2023",)"
    R"("Protocol":"tcp","WaitOp":"50","WaitTimeout":"400"})";

TEST(FinCoreCompat, NumericInitLibSendsTheLogPathHandshake) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    EXPECT_EQ(rig.session.init(kNumericInit), 1);
    EXPECT_EQ(rig.wire->take(), std::vector<std::string>{R"({"ChartId":131,"Id":1026,"Sym":""})"});
    EXPECT_EQ(rig.wire->host, "127.0.0.1");
    EXPECT_EQ(rig.wire->port, 2022);
    EXPECT_EQ(rig.wire->wait_timeout_ms, 400);
    rig.session.destroy(131);
}

TEST(FinCoreCompat, StringInitLibSendsTheLogPathHandshake) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    EXPECT_EQ(rig.session.init(kStringInit), 1);
    EXPECT_EQ(rig.wire->take(),
              std::vector<std::string>{R"({"ChartId":133000000000000001,"Id":1026,"Sym":""})"});
    EXPECT_EQ(rig.wire->host, "10.0.0.5");
    EXPECT_EQ(rig.wire->port, 2023);
    EXPECT_EQ(rig.wire->wait_timeout_ms, 400);
    rig.session.destroy(133000000000000001);
}

TEST(FinCoreCompat, HandshakeWithoutAnswerStillConnects) {
    Rig rig;  // no responder: the 1026 request times out after WaitTimeout
    EXPECT_EQ(rig.session.init(kNumericInit), 1);
    EXPECT_EQ(rig.wire->take().size(), 1u);
    rig.session.destroy(131);
}

TEST(FinCoreCompat, EmptyInitLibSendsNothing) {
    Rig rig;
    EXPECT_EQ(rig.session.init(""), 1);
    EXPECT_EQ(rig.session.init("{}"), 1);
    EXPECT_TRUE(rig.wire->take().empty());
    rig.session.destroy(0);
    rig.session.destroy(0);
}

TEST(FinCoreCompat, LaterInitLibSharesTheSessionWithoutAnotherHandshake) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    ASSERT_EQ(rig.session.init(kStringInit), 1);
    EXPECT_EQ(rig.wire->take().size(), 1u);
    EXPECT_EQ(rig.wire->transports_created, 1);
    EXPECT_EQ(rig.session.references(), 2);
    rig.session.destroy(131);
    rig.session.destroy(133000000000000001);
}

TEST(FinCoreCompat, BeforeInitLib) {
    Rig rig;
    EXPECT_EQ(rig.session.get_levels("EURUSD", 1), "Failed to initialize MQLConnector.dll");
    EXPECT_EQ(rig.session.get_signal_cache("k", "EURUSD", 1), "Failed to initialize MQLConnector.dll");
    EXPECT_EQ(rig.session.send_and_wait(R"({"Id":1001,"ChartId":1})"), "");
    rig.session.post(R"({"Id":1001,"ChartId":1})");
    EXPECT_EQ(rig.session.listen(1), "");
    EXPECT_EQ(rig.wire->transports_created, 0);
}

TEST(FinCoreCompat, GetLevelsFrameAndReturnedData) {
    Rig rig;
    std::string data = R"("1.1;2.2")";
    rig.wire->respond = [&](std::string_view sent) -> std::vector<std::string> {
        if (sent.find("\"Id\":1023") == std::string_view::npos) return answer_handshake(sent);
        return {R"({"Id":1023,"ChartId":131,"Data":)" + data + "}"};
    };
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    (void)rig.wire->take();

    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "1.1;2.2");
    EXPECT_EQ(rig.wire->take(), std::vector<std::string>{R"({"ChartId":131,"Id":1023,"Sym":"EURUSD"})"});

    data = R"({"levels":[1.1,2.2]})";
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), R"({"levels":[1.1,2.2]})");
    data = "null";
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "");
    data = "42.5";
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "42.5");
    data = "\"a\\\"b\\n\\u20ac\"";  // ordinary literal: see signal_json_test
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "a\"b\n\xE2\x82\xAC");
    (void)rig.wire->take();

    // Sym is escaped by the writer: quotes, control characters (0x1A included); UTF-8 as is.
    EXPECT_EQ(rig.session.get_levels("q\"\x1A\xE2\x82\xAC", 131), "a\"b\n\xE2\x82\xAC");
    EXPECT_EQ(rig.wire->take(),
              std::vector<std::string>{"{\"ChartId\":131,\"Id\":1023,\"Sym\":\"q\\\"\\u001a\xE2\x82\xAC\"}"});
    rig.session.destroy(131);
}

TEST(FinCoreCompat, GetLevelsTimesOutToEmpty) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "");
    rig.session.destroy(131);
}

TEST(FinCoreCompat, GetSignalCacheFrameAndWholeReply) {
    Rig rig;
    const std::string reply = R"({ "Id" : 1033, "ChartId" : 131, "Data" : {"k":[1,2]}, "Extra":"x" })";
    rig.wire->respond = [&](std::string_view sent) -> std::vector<std::string> {
        if (sent.find("\"Id\":1033") == std::string_view::npos) return answer_handshake(sent);
        return {reply};
    };
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    (void)rig.wire->take();

    EXPECT_EQ(rig.session.get_signal_cache("key-1", "EURUSD", 131), reply);
    EXPECT_EQ(rig.wire->take(),
              std::vector<std::string>{R"({"ChartId":131,"Id":1033,"Sym":"EURUSD","Key":"key-1"})"});
    rig.session.destroy(131);
}

TEST(FinCoreCompat, DoSendMessageSendsVerbatimAndReturnsTheFrameVerbatim) {
    Rig rig;
    const std::string reply = "{ \"Id\":1001, \"ChartId\":131, \"Data\":\"pong \\u001a\" }";
    rig.wire->respond = [&](std::string_view sent) -> std::vector<std::string> {
        if (sent.find("\"Id\":1001") == std::string_view::npos) return answer_handshake(sent);
        return {reply};
    };
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    (void)rig.wire->take();

    const std::string request = R"({"Sym":"EURUSD", "Id":1001,"ChartId":131,"Data":{"a":[1,{"b":null}]}})";
    EXPECT_EQ(rig.session.send_and_wait(request), reply);
    EXPECT_EQ(rig.wire->take(), std::vector<std::string>{request});

    // A raw 0x1A inside a string (accepted, as Newtonsoft accepts raw control characters) is
    // re-escaped so it cannot end the frame early; nothing else changes.
    EXPECT_EQ(rig.session.send_and_wait("{\"Id\":1001,\"ChartId\":131,\"Data\":\"a\x1A" "b\"}"), reply);
    EXPECT_EQ(rig.wire->take(),
              std::vector<std::string>{"{\"Id\":1001,\"ChartId\":131,\"Data\":\"a\\u001ab\"}"});

    // String ChartId routes like a number; no ChartId routes to 0.
    EXPECT_EQ(rig.session.send_and_wait(R"({"Id":1001,"ChartId":"131"})"), reply);
    rig.session.destroy(131);
}

TEST(FinCoreCompat, DoSendMessageRejectsNonObjects) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    (void)rig.wire->take();
    EXPECT_EQ(rig.session.send_and_wait(""), "");
    EXPECT_EQ(rig.session.send_and_wait("not json"), "");
    EXPECT_EQ(rig.session.send_and_wait("{\"Id\":1001"), "");
    EXPECT_TRUE(rig.wire->take().empty());
    rig.session.destroy(131);
}

TEST(FinCoreCompat, DoSendMessageReturnsTheFirstFrameQueuedForTheChart) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    // A broadcast already queued for the chart is what the next DoSendMessage returns, and the
    // real reply stays queued for DoListenMessage (no correlation in FinCore's protocol).
    rig.wire->deliver(R"({"Id":1018,"ChartId":131,"Data":"broadcast"})");
    rig.wire->respond = [](std::string_view) -> std::vector<std::string> {
        return {R"({"Id":1001,"ChartId":131,"Data":"reply","ReqId":7})"};
    };
    EXPECT_EQ(rig.session.send_and_wait(R"({"Id":1001,"ChartId":131,"ReqId":7})"),
              R"({"Id":1018,"ChartId":131,"Data":"broadcast"})");
    EXPECT_EQ(rig.session.listen(131), R"({"Id":1001,"ChartId":131,"Data":"reply","ReqId":7})");
    EXPECT_EQ(rig.session.listen(131), "");
    rig.session.destroy(131);
}

TEST(FinCoreCompat, DoPostMessageSendsVerbatim) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kStringInit), 1);
    (void)rig.wire->take();
    rig.wire->respond = nullptr;

    rig.session.post(R"({"Id":1013,"ChartId":133000000000000001,"Data":"log line"})");
    rig.session.post("{\"Id\":1013,\"Data\":\"x\x1Ay\"}");
    rig.session.post("{broken");
    rig.session.post("");
    EXPECT_EQ(rig.wire->take(),
              (std::vector<std::string>{R"({"Id":1013,"ChartId":133000000000000001,"Data":"log line"})",
                                        "{\"Id\":1013,\"Data\":\"x\\u001ay\"}"}));
    rig.session.destroy(133000000000000001);
}

TEST(FinCoreCompat, DoListenMessageReturnsQueuedFramesInOrder) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    ASSERT_EQ(rig.session.init(R"({"ChartId":0})"), 1);

    rig.wire->deliver(R"({"Id":1018,"ChartId":131,"Data":"first"})");
    rig.wire->deliver(R"({"Id":1018,"ChartId":999,"Data":"nobody subscribed"})");
    rig.wire->deliver(R"( {"Id":1018,"ChartId":"131","Data":"second"} )");
    rig.wire->deliver(R"({"Id":1018,"Data":"no ChartId: chart 0"})");
    rig.wire->deliver("not json");
    rig.wire->deliver("  ");

    EXPECT_EQ(rig.session.listen(131), R"({"Id":1018,"ChartId":131,"Data":"first"})");
    EXPECT_EQ(rig.session.listen(131), R"( {"Id":1018,"ChartId":"131","Data":"second"} )");
    EXPECT_EQ(rig.session.listen(131), "");
    EXPECT_EQ(rig.session.listen(999), "");
    EXPECT_EQ(rig.session.listen(0), R"({"Id":1018,"Data":"no ChartId: chart 0"})");
    EXPECT_EQ(rig.session.listen(0), "");
    rig.session.destroy(0);
    rig.session.destroy(131);
}

TEST(FinCoreCompat, DestroyLibLastReferenceStopsAndInitLibStartsOver) {
    Rig rig;
    rig.wire->respond = answer_handshake;
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    rig.session.destroy(131);
    EXPECT_EQ(rig.session.get_levels("EURUSD", 131), "Failed to initialize MQLConnector.dll");
    ASSERT_EQ(rig.session.init(kNumericInit), 1);
    EXPECT_EQ(rig.wire->transports_created, 2);
    EXPECT_EQ(rig.wire->take().size(), 2u);  // one handshake per connection lifetime
    rig.session.destroy(131);
}

}  // namespace
}  // namespace mqlc::connector
