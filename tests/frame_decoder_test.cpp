#include "core/frame_decoder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mqlc::core {
namespace {

struct Collector {
    FrameDecoder decoder;
    std::vector<std::string> frames;

    explicit Collector(std::size_t cap = kMaxFrameBytes) : decoder(cap) {}

    std::size_t feed(std::string_view bytes) {
        return decoder.feed(std::span<const char>(bytes.data(), bytes.size()),
                            [this](std::string_view f) { frames.emplace_back(f); });
    }
};

TEST(FrameDecoder, SplitsOneFrameAcrossReads) {
    Collector c;
    c.feed(R"({"Id":10)");
    c.feed(R"(26,"ChartId")");
    EXPECT_TRUE(c.frames.empty());
    c.feed(":7}\x1A");
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], R"({"Id":1026,"ChartId":7})");
    EXPECT_EQ(c.decoder.pending_bytes(), 0u);
}

TEST(FrameDecoder, SeveralFramesInOneRead) {
    Collector c;
    c.feed("{\"a\":1}\x1A{\"b\":2}\x1A{\"c\":");
    ASSERT_EQ(c.frames.size(), 2u);
    EXPECT_EQ(c.frames[0], "{\"a\":1}");
    EXPECT_EQ(c.frames[1], "{\"b\":2}");
    c.feed("3}\x1A");
    ASSERT_EQ(c.frames.size(), 3u);
    EXPECT_EQ(c.frames[2], "{\"c\":3}");
}

TEST(FrameDecoder, EmptyFramesAreReported) {
    Collector c;
    c.feed("\x1A\x1A");
    ASSERT_EQ(c.frames.size(), 2u);
    EXPECT_TRUE(c.frames[0].empty());
}

TEST(FrameDecoder, Utf8MultibyteSplitAcrossReads) {
    // U+20AC is E2 82 AC; cut after its first byte.
    const std::string frame = "{\"Sym\":\"\xE2\x82\xAC\"}";
    Collector c;
    c.feed(std::string_view(frame).substr(0, 9));
    c.feed(std::string_view(frame).substr(9));
    c.feed("\x1A");
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], frame);
}

TEST(FrameDecoder, ByteByByte) {
    const std::string stream = "{\"x\":\"y\"}\x1A{}\x1A";
    Collector c;
    for (const char ch : stream) c.feed(std::string_view(&ch, 1));
    ASSERT_EQ(c.frames.size(), 2u);
    EXPECT_EQ(c.frames[1], "{}");
}

TEST(FrameDecoder, OversizedFrameDroppedThenRecovers) {
    Collector c(16);
    EXPECT_EQ(c.feed("0123456789"), 0u);
    EXPECT_EQ(c.feed("0123456789"), 1u);  // 20 > 16: dropped
    EXPECT_EQ(c.decoder.pending_bytes(), 0u);
    c.feed("still the same oversized frame");
    c.feed("tail\x1A{\"ok\":1}\x1A");
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], "{\"ok\":1}");
    EXPECT_EQ(c.decoder.dropped_total(), 1u);
}

TEST(FrameDecoder, OversizedCompleteFrameInOneRead) {
    Collector c(4);
    EXPECT_EQ(c.feed("toolong\x1A{}\x1A"), 1u);
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], "{}");
}

TEST(FrameDecoder, FrameAtExactlyTheCapIsKept) {
    Collector c(4);
    c.feed("ab");
    c.feed("cd\x1A");
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], "abcd");
}

TEST(FrameDecoder, ResetDiscardsPartialFrame) {
    Collector c;
    c.feed("{\"half\":");
    c.decoder.reset();
    c.feed("{}\x1A");
    ASSERT_EQ(c.frames.size(), 1u);
    EXPECT_EQ(c.frames[0], "{}");
}

}  // namespace
}  // namespace mqlc::core
