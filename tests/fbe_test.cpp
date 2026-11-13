// Protocol "fbe": FbeCodec's fixed-offset envelope reads, length framing of FBE messages
// (split / coalesced / bad sizes), the configuration it forces, and a Session over TCP loopback
// with ReqId correlation and 8 concurrent routes. Messages are tests/fbe/test.fbe TestMessage.
#include "connector/session.h"
#include "core/codec.h"
#include "core/config.h"
#include "core/framer.h"
#include "fbe/fbe_message.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace mqlc {
namespace {

using namespace mqlc::test;
using core::FbeCodec;
namespace fbe = mqlc::test::fbe;

fbe::TestMessage message(std::int64_t chart, std::int64_t req, std::int32_t id, std::string payload = "p") {
    fbe::TestMessage m;
    m.chart_id = chart;
    m.req_id = req;
    m.id = id;
    m.sym = "EURUSD";
    m.payload = std::move(payload);
    return m;
}

/// What the framer hands the codec: the message without its u32 size.
std::string frame_of(const fbe::TestMessage& m) { return fbe::encode(m).substr(4); }

std::string length_init(std::uint16_t port, std::int64_t chart, int wait_ms = 2000) {
    return core::json::ObjectWriter()
        .add_string("Host", "127.0.0.1")
        .add_int("Port", port)
        .add_int("ChartId", chart)
        .add_int("WaitTimeout", wait_ms)
        .add_string("Protocol", "fbe")
        .finish();
}

core::FramingOptions length_framing() {
    core::FramingOptions options;
    options.kind = core::FramingKind::length;
    return options;
}

std::optional<std::string> mirror(std::string_view request) { return std::string(request); }

// --- FbeCodec ---------------------------------------------------------------------------------

TEST(FbeCodec, ReadsRouteCorrelationAndIdAtFixedOffsets) {
    const FbeCodec codec;
    const auto envelope = codec.inspect(frame_of(message(133000000000000001, 0x0102030405060708, 1039)));
    ASSERT_TRUE(envelope);
    EXPECT_EQ(envelope->route, 133000000000000001);
    ASSERT_TRUE(envelope->corr);
    EXPECT_EQ(std::get<std::int64_t>(*envelope->corr), 0x0102030405060708);
    EXPECT_EQ(envelope->id, 1039);
    EXPECT_TRUE(codec.correlates());
}

TEST(FbeCodec, NegativeValuesAndZeroReqId) {
    const FbeCodec codec;
    const auto envelope = codec.inspect(frame_of(message(-5, 0, -1)));
    ASSERT_TRUE(envelope);
    EXPECT_EQ(envelope->route, -5);
    EXPECT_FALSE(envelope->corr);  // ReqId 0: uncorrelated
    EXPECT_EQ(envelope->id, -1);
    const auto max = codec.inspect(frame_of(message(std::numeric_limits<std::int64_t>::max(),
                                                    std::numeric_limits<std::int64_t>::min(), 7)));
    ASSERT_TRUE(max && max->corr);
    EXPECT_EQ(max->route, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(std::get<std::int64_t>(*max->corr), std::numeric_limits<std::int64_t>::min());
}

TEST(FbeCodec, TruncatedFramesAreRejected) {
    const FbeCodec codec;
    const std::string frame = frame_of(message(1, 2, 3));
    for (std::size_t size = 0; size < FbeCodec::kMinFrame; ++size)
        EXPECT_FALSE(codec.inspect(std::string_view(frame).substr(0, size))) << size;
    EXPECT_TRUE(codec.inspect(std::string_view(frame).substr(0, FbeCodec::kMinFrame)));
}

TEST(FbeCodec, ProtectNeverCopies) {
    const FbeCodec codec;
    const std::string frame = frame_of(message(1, 2, 3, std::string("\x1A\x00\x1A", 3)));
    std::string storage;
    const auto out = codec.protect(frame, std::nullopt, storage);
    EXPECT_EQ(out.data(), frame.data());
    EXPECT_TRUE(storage.empty());
}

// --- Length framing of FBE messages ----------------------------------------------------------

struct Collect final : core::FrameSink {
    std::vector<std::string> frames;
    void on_frame(std::string_view frame) override { frames.emplace_back(frame); }
};

TEST(FbeFraming, CoalescedAndSplitMessagesComeOutWhole) {
    const std::string a = fbe::encode(message(11, 1, 1039, "first"));
    const std::string b = fbe::encode(message(12, 2, 1002, std::string(70'000, 'x')));
    const std::string c = fbe::encode(message(13, 3, 1005, ""));
    const std::string stream = a + b + c;

    // All three in one read.
    {
        core::LengthPrefixFramer framer;
        Collect sink;
        EXPECT_FALSE(framer.feed(std::span<const char>(stream.data(), stream.size()), sink).corrupt);
        ASSERT_EQ(sink.frames.size(), 3u);
        EXPECT_EQ(sink.frames[0], a.substr(4));
        EXPECT_EQ(sink.frames[1], b.substr(4));
        EXPECT_EQ(sink.frames[2], c.substr(4));
    }
    // Byte by byte, and in odd-sized chunks: routes and correlation survive every split.
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{4093}}) {
        core::LengthPrefixFramer framer;
        Collect sink;
        for (std::size_t at = 0; at < stream.size(); at += chunk) {
            const std::size_t n = std::min(chunk, stream.size() - at);
            ASSERT_FALSE(framer.feed(std::span<const char>(stream.data() + at, n), sink).corrupt);
        }
        ASSERT_EQ(sink.frames.size(), 3u) << chunk;
        const FbeCodec codec;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto envelope = codec.inspect(sink.frames[i]);
            ASSERT_TRUE(envelope);
            EXPECT_EQ(envelope->route, 11 + static_cast<std::int64_t>(i));
            EXPECT_EQ(std::get<std::int64_t>(*envelope->corr), 1 + static_cast<std::int64_t>(i));
        }
        EXPECT_EQ(fbe::decode(fbe::reframe(sink.frames[1]))->payload.size(), 70'000u);
    }
}

TEST(FbeFraming, WrongSizesMakeTheStreamCorrupt) {
    for (const std::uint32_t bad : {0u, 3u, static_cast<std::uint32_t>(core::kMaxFrameBytes + 5)}) {
        std::string stream = fbe::encode(message(1, 1, 1));
        for (int i = 0; i < 4; ++i) stream[static_cast<std::size_t>(i)] = static_cast<char>((bad >> (8 * i)) & 0xFF);
        core::LengthPrefixFramer framer;
        Collect sink;
        EXPECT_TRUE(framer.feed(std::span<const char>(stream.data(), stream.size()), sink).corrupt) << bad;
        EXPECT_TRUE(sink.frames.empty());
    }
}

// --- Configuration ----------------------------------------------------------------------------

TEST(FbeConfig, ForcesLengthFramingAndDefaultsToPort2024) {
    const auto parsed = core::parse_config(R"({"Protocol":"fbe","ChartId":1})");
    EXPECT_TRUE(parsed.config.fbe());
    EXPECT_EQ(parsed.config.framing, core::FramingKind::length);
    EXPECT_EQ(parsed.config.port, 2024);
    EXPECT_TRUE(parsed.warnings.empty());

    const auto explicit_port = core::parse_config(R"({"Protocol":"FBE","Port":"3000","Framing":"delimiter"})");
    EXPECT_TRUE(explicit_port.config.fbe());
    EXPECT_EQ(explicit_port.config.port, 3000);
    EXPECT_EQ(explicit_port.config.framing, core::FramingKind::length);
    ASSERT_EQ(explicit_port.warnings.size(), 1u);
    EXPECT_NE(explicit_port.warnings[0].find("needs Framing \"length\""), std::string::npos);

    EXPECT_FALSE(core::parse_config(R"({"Protocol":"tcp"})").config.fbe());
    EXPECT_EQ(core::parse_config(R"({"Protocol":"tcp"})").config.port, 2022);
}

// --- Session over TCP loopback ----------------------------------------------------------------

TEST(FbeSession, SendBinaryRoundTrip) {
    tools::LoopbackServer server(0, mirror, length_framing());
    ASSERT_TRUE(server.listening());
    connector::Session session;
    ASSERT_EQ(session.init(length_init(server.port(), 7)), 1);

    const std::string request = fbe::encode(message(7, 42, 1039, std::string("bin\x1A\0ary", 8)));
    const auto reply = session.send_binary(request);
    ASSERT_TRUE(reply);
    EXPECT_EQ(fbe::reframe(*reply), request);  // the whole message MQL decodes
    std::string framed(connector::Session::framed_size(*reply), '\0');
    connector::Session::write_framed(*reply, reinterpret_cast<unsigned char*>(framed.data()));
    EXPECT_EQ(framed, request);

    // Posted messages arrive through listen_binary; the text API leaves them alone.
    session.post_binary(fbe::encode(message(7, 0, 1001, "posted")));
    std::optional<std::string> heard;
    EXPECT_EQ(session.listen(7), "");
    ASSERT_TRUE(eventually([&] { return (heard = session.listen_binary(7)).has_value(); }));
    EXPECT_EQ(fbe::decode(fbe::reframe(*heard))->payload, "posted");
    session.destroy(7);
}

TEST(FbeSession, BadInputIsNotSent) {
    tools::LoopbackServer server(0, mirror, length_framing());
    connector::Session session;
    ASSERT_EQ(session.init(length_init(server.port(), 3, 300)), 1);
    std::string request = fbe::encode(message(3, 1, 1));
    EXPECT_FALSE(session.send_binary(request.substr(0, request.size() - 1)));  // size field disagrees
    EXPECT_FALSE(session.send_binary(""));
    EXPECT_FALSE(session.send_binary(std::string("\x08\0\0\0\x01\0\0\0", 8)));  // no envelope
    EXPECT_EQ(session.send_and_wait(R"({"ChartId":3})"), "");  // JSON on the binary protocol
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(server.requests(), 0u);
    session.destroy(3);

    // Binary calls on a JSON connection do nothing.
    tools::LoopbackServer json_server(0, fincore_like());
    connector::Session json_session;
    ASSERT_EQ(json_session.init(init_json(json_server.port(), 4)), 1);
    EXPECT_FALSE(json_session.send_binary(fbe::encode(message(4, 1, 1))));
    EXPECT_FALSE(json_session.listen_binary(4));
    json_session.destroy(4);
}

TEST(FbeSession, CorrelatedWaitIgnoresOtherReqIds) {
    // The server answers with another ReqId: the request times out, and the stray reply stays
    // queued on the route for DoListenBinary.
    const auto wrong_req = [](std::string_view request) -> std::optional<std::string> {
        auto m = fbe::decode(fbe::reframe(request));
        if (!m) return std::nullopt;
        m->req_id += 1000;
        return fbe::encode(*m).substr(4);
    };
    tools::LoopbackServer server(0, wrong_req, length_framing());
    connector::Session session;
    ASSERT_EQ(session.init(length_init(server.port(), 9, 200)), 1);
    EXPECT_FALSE(session.send_binary(fbe::encode(message(9, 5, 1039))));
    const auto stray = session.listen_binary(9);
    ASSERT_TRUE(stray);
    EXPECT_EQ(fbe::decode(fbe::reframe(*stray))->req_id, 1005);
    session.destroy(9);
}

TEST(FbeSession, EightRoutesOnEightThreadsCorrelateExactly) {
    tools::LoopbackServer server(0, mirror, length_framing());
    connector::Session session;
    ASSERT_EQ(session.init(length_init(server.port(), 200)), 1);
    constexpr int kRoutes = 8;
    constexpr int kRequests = 200;
    std::atomic<int> mismatches{0};
    {
        std::vector<std::jthread> threads;
        for (int r = 0; r < kRoutes; ++r) {
            threads.emplace_back([&, r] {
                const std::int64_t route = 200 + r;
                for (int i = 0; i < kRequests; ++i) {
                    const std::int64_t req = route * 100'000 + i + 1;
                    const std::string tag = std::to_string(route) + "/" + std::to_string(i);
                    const auto reply = session.send_binary(fbe::encode(message(route, req, 1039, tag)));
                    const auto decoded = reply ? fbe::decode(fbe::reframe(*reply)) : std::nullopt;
                    if (!decoded || decoded->chart_id != route || decoded->req_id != req || decoded->payload != tag)
                        ++mismatches;
                }
            });
        }
    }
    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_EQ(server.requests(), static_cast<std::uint64_t>(kRoutes * kRequests));  // no JSON handshake
    session.destroy(200);
}

}  // namespace
}  // namespace mqlc
