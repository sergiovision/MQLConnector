#include "net/transport.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

namespace mqlc::net {
namespace {

using namespace mqlc::test;
using Clock = std::chrono::steady_clock;

struct Received {
    std::mutex mutex;
    std::vector<std::string> frames;

    Transport::FrameHandler handler() {
        return [this](std::string_view frame) {
            const std::scoped_lock lock(mutex);
            frames.emplace_back(frame);
        };
    }
    std::size_t count() {
        const std::scoped_lock lock(mutex);
        return frames.size();
    }
};

std::unique_ptr<Transport> connect_to(std::uint16_t port, Received& received) {
    TcpOptions options;
    options.port = port;
    return make_tcp_transport(options, received.handler(), nullptr);
}

TEST(TcpTransport, SendsAndReceivesFrames) {
    tools::LoopbackServer server;
    Received received;
    const auto transport = connect_to(server.port(), received);
    ASSERT_TRUE(transport->connect(2000ms));
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(transport->send(R"({"Id":1001,"ChartId":5})"));
    ASSERT_TRUE(eventually([&] { return received.count() == 100; }));
    EXPECT_EQ(member(received.frames[0], "ChartId"), "5");
    EXPECT_EQ(member(received.frames[99], "Data"), "echo");
}

TEST(TcpTransport, RejectsFrameContainingDelimiter) {
    tools::LoopbackServer server;
    Received received;
    const auto transport = connect_to(server.port(), received);
    ASSERT_TRUE(transport->connect(2000ms));
    EXPECT_FALSE(transport->send("{}\x1A{}"));
}

TEST(TcpTransport, LengthFramingRoundTrip) {
    core::FramingOptions length;
    length.kind = core::FramingKind::length;
    tools::LoopbackServer server(0, tools::LoopbackServer::echo, length);
    Received received;
    TcpOptions options;
    options.port = server.port();
    options.framing = length;
    const auto transport = make_tcp_transport(options, received.handler(), nullptr);
    ASSERT_TRUE(transport->connect(2000ms));
    // The 0x1A delimiter means nothing to a length-prefixed stream.
    for (int i = 0; i < 50; ++i) ASSERT_TRUE(transport->send("{\"Id\":1,\"ChartId\":2,\"Data\":\"\x1A\"}"));
    ASSERT_TRUE(eventually([&] { return received.count() == 50; }));
    EXPECT_EQ(member(received.frames[49], "ChartId"), "2");
}

TEST(TcpTransport, SendWhileDisconnectedIsDropped) {
    Received received;
    const auto transport = connect_to(free_port(), received);
    EXPECT_FALSE(transport->connect(100ms));
    EXPECT_FALSE(transport->connected());
    EXPECT_FALSE(transport->send("{}"));
}

TEST(TcpTransport, StopIsBoundedWhileReconnecting) {
    Received received;
    const auto transport = connect_to(free_port(), received);
    std::this_thread::sleep_for(300ms);  // inside the reconnect loop now
    const auto start = Clock::now();
    transport->stop();
    EXPECT_LT(Clock::now() - start, 500ms);
    transport->stop();  // idempotent
}

TEST(TcpTransport, StopIsBoundedWhileConnected) {
    tools::LoopbackServer server;
    Received received;
    const auto transport = connect_to(server.port(), received);
    ASSERT_TRUE(transport->connect(2000ms));
    const auto start = Clock::now();
    transport->stop();
    EXPECT_LT(Clock::now() - start, 500ms);
    EXPECT_FALSE(transport->connected());
}

TEST(TcpTransport, ReconnectsAfterServerRestart) {
    auto server = std::make_unique<tools::LoopbackServer>();
    const std::uint16_t port = server->port();
    Received received;
    const auto transport = connect_to(port, received);
    ASSERT_TRUE(transport->connect(2000ms));
    server.reset();
    ASSERT_TRUE(eventually([&] { return !transport->connected(); }));
    server = std::make_unique<tools::LoopbackServer>(port);
    ASSERT_TRUE(server->listening()) << server->error();
    ASSERT_TRUE(transport->connect(3000ms));
    ASSERT_TRUE(transport->send(R"({"Id":1,"ChartId":1})"));
    EXPECT_TRUE(eventually([&] { return received.count() == 1; }));
}

}  // namespace
}  // namespace mqlc::net
