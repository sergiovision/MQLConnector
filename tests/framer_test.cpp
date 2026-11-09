// The Framer interface: LengthPrefixFramer (FBE layout) and DelimiterFramer through the
// interface. The delimiter splitter's own edge cases are in frame_decoder_test.cpp.
#include "core/framer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mqlc::core {
namespace {

struct Frames final : FrameSink {
    std::vector<std::string> got;
    std::vector<const char*> addresses;  // where each view pointed, to check zero-copy delivery
    void on_frame(std::string_view frame) override {
        got.emplace_back(frame);
        addresses.push_back(frame.data());
    }
};

FeedResult feed(Framer& framer, std::string_view bytes, FrameSink& sink) {
    return framer.feed(std::span<const char>(bytes.data(), bytes.size()), sink);
}

/// [u32 LE total][payload]
std::string length_frame(std::string_view payload) {
    std::string out;
    EXPECT_TRUE(LengthPrefixFramer().encode(payload, out));
    return out;
}

std::string raw_prefix(std::uint32_t total) {
    std::string out;
    LengthPrefixFramer::append_u32_le(out, total);
    return out;
}

TEST(LengthPrefixFramer, EncodesLittleEndianTotalIncludingThePrefix) {
    std::string out;
    ASSERT_TRUE(LengthPrefixFramer().encode("abc", out));
    EXPECT_EQ(out, std::string("\x07\x00\x00\x00" "abc", 7));

    out.clear();
    const std::string payload(300, 'x');
    ASSERT_TRUE(LengthPrefixFramer().encode(payload, out));
    EXPECT_EQ(static_cast<unsigned char>(out[0]), (300 + 4) & 0xFF);
    EXPECT_EQ(static_cast<unsigned char>(out[1]), (300 + 4) >> 8);
    EXPECT_EQ(out[2], 0);
    EXPECT_EQ(out[3], 0);

    // The byte order is explicit, independent of the host's.
    EXPECT_EQ(raw_prefix(0x01020304), std::string("\x04\x03\x02\x01", 4));
    EXPECT_EQ(LengthPrefixFramer::read_u32_le("\x04\x03\x02\x01"), 0x01020304u);
    EXPECT_EQ(LengthPrefixFramer::read_u32_le("\xFF\xFF\xFF\xFF"), 0xFFFFFFFFu);
}

TEST(LengthPrefixFramer, EmptyPayloadIsFourBytes) {
    std::string out;
    ASSERT_TRUE(LengthPrefixFramer().encode("", out));
    EXPECT_EQ(out, std::string("\x04\x00\x00\x00", 4));
    LengthPrefixFramer framer;
    Frames sink;
    EXPECT_FALSE(feed(framer, out, sink).corrupt);
    ASSERT_EQ(sink.got.size(), 1u);
    EXPECT_TRUE(sink.got[0].empty());
}

TEST(LengthPrefixFramer, ManyFramesInOneReadAreViewsIntoTheBuffer) {
    const std::string stream = length_frame(R"({"a":1})") + length_frame("") + length_frame(R"({"b":2})");
    LengthPrefixFramer framer;
    Frames sink;
    EXPECT_FALSE(feed(framer, stream, sink).corrupt);
    ASSERT_EQ(sink.got, (std::vector<std::string>{R"({"a":1})", "", R"({"b":2})"}));
    EXPECT_EQ(sink.addresses[0], stream.data() + 4);
    EXPECT_EQ(sink.addresses[2], stream.data() + 4 + 7 + 4 + 4);
    EXPECT_EQ(framer.pending_bytes(), 0u);
}

TEST(LengthPrefixFramer, HeaderSplitAcrossReads) {
    const std::string stream = length_frame(R"({"Id":1026})");
    LengthPrefixFramer framer;
    Frames sink;
    for (const std::size_t cut : {1u, 2u, 3u}) {
        feed(framer, std::string_view(stream).substr(0, cut), sink);
        EXPECT_TRUE(sink.got.empty());
        feed(framer, std::string_view(stream).substr(cut), sink);
        ASSERT_EQ(sink.got.size(), 1u) << cut;
        EXPECT_EQ(sink.got[0], R"({"Id":1026})");
        sink.got.clear();
    }
}

TEST(LengthPrefixFramer, PayloadSplitAcrossReadsAndByteByByte) {
    const std::string stream = length_frame("hello") + length_frame("world!");
    {
        LengthPrefixFramer framer;
        Frames sink;
        feed(framer, std::string_view(stream).substr(0, 6), sink);  // prefix + "he"
        EXPECT_TRUE(sink.got.empty());
        EXPECT_EQ(framer.pending_bytes(), 2u);
        feed(framer, std::string_view(stream).substr(6), sink);
        EXPECT_EQ(sink.got, (std::vector<std::string>{"hello", "world!"}));
    }
    {
        LengthPrefixFramer framer;
        Frames sink;
        for (const char c : stream) EXPECT_FALSE(feed(framer, std::string_view(&c, 1), sink).corrupt);
        EXPECT_EQ(sink.got, (std::vector<std::string>{"hello", "world!"}));
    }
}

TEST(LengthPrefixFramer, BinaryPayloadKeepsEveryByte) {
    const std::string payload("\x00\x1A\n\xFF{}", 6);
    LengthPrefixFramer framer;
    Frames sink;
    feed(framer, length_frame(payload), sink);
    ASSERT_EQ(sink.got.size(), 1u);
    EXPECT_EQ(sink.got[0], payload);
}

TEST(LengthPrefixFramer, SizeBelowFourIsCorruptThenResumesAfterReset) {
    for (const std::uint32_t bad : {0u, 1u, 3u}) {
        LengthPrefixFramer framer;
        Frames sink;
        // A good frame, then a bad prefix in the same read: the good one is delivered, the
        // rest of the read (whatever follows the bad prefix) is not.
        const std::string stream = length_frame("ok") + raw_prefix(bad) + length_frame("lost");
        const FeedResult result = feed(framer, stream, sink);
        EXPECT_TRUE(result.corrupt) << bad;
        EXPECT_EQ(sink.got, std::vector<std::string>{"ok"});
        EXPECT_EQ(framer.pending_bytes(), 0u);

        // The transport reconnects and resets: the new stream frames cleanly.
        framer.reset();
        EXPECT_FALSE(feed(framer, length_frame("after reconnect"), sink).corrupt);
        EXPECT_EQ(sink.got.back(), "after reconnect");
    }
}

TEST(LengthPrefixFramer, OverTheCapIsCorrupt) {
    LengthPrefixFramer framer(16);
    Frames sink;
    EXPECT_FALSE(feed(framer, length_frame(std::string(16, 'x')), sink).corrupt);  // exactly the cap
    ASSERT_EQ(sink.got.size(), 1u);
    EXPECT_TRUE(feed(framer, raw_prefix(16 + 4 + 1), sink).corrupt);
    EXPECT_TRUE(feed(framer, raw_prefix(0xFFFFFFFFu), sink).corrupt);
    // A split header is judged once complete.
    const std::string big = raw_prefix(1000);
    EXPECT_FALSE(feed(framer, big.substr(0, 2), sink).corrupt);
    EXPECT_TRUE(feed(framer, big.substr(2), sink).corrupt);
    framer.reset();
    EXPECT_FALSE(feed(framer, length_frame("fine"), sink).corrupt);
    EXPECT_EQ(sink.got.back(), "fine");
}

TEST(LengthPrefixFramer, ResetDiscardsPartialFrame) {
    LengthPrefixFramer framer;
    Frames sink;
    feed(framer, length_frame("abcdef").substr(0, 7), sink);
    EXPECT_GT(framer.pending_bytes(), 0u);
    framer.reset();
    EXPECT_EQ(framer.pending_bytes(), 0u);
    feed(framer, length_frame("xy"), sink);
    EXPECT_EQ(sink.got, std::vector<std::string>{"xy"});
}

TEST(DelimiterFramer, ThroughTheInterfaceWithAnotherDelimiter) {
    const auto framer = make_framer(FramingOptions{FramingKind::delimiter, '\n', kMaxFrameBytes});
    EXPECT_EQ(framer->delimiter(), '\n');
    EXPECT_EQ(framer->overhead(), 1u);
    std::string out;
    EXPECT_TRUE(framer->encode(R"({"a":1})", out));
    EXPECT_EQ(out, "{\"a\":1}\n");
    EXPECT_FALSE(framer->encode("{\"a\":\n1}", out));  // the payload holds the delimiter
    EXPECT_TRUE(framer->encode("x\x1Ay", out));         // 0x1A is ordinary data here

    Frames sink;
    const std::string stream = "{\"a\":1}\n{\"b\":\x1A}\n{\"c\"";
    EXPECT_FALSE(feed(*framer, stream, sink).corrupt);
    EXPECT_EQ(sink.got, (std::vector<std::string>{"{\"a\":1}", "{\"b\":\x1A}"}));
    feed(*framer, ":3}\n", sink);
    EXPECT_EQ(sink.got.back(), "{\"c\":3}");
}

TEST(DelimiterFramer, OversizedFrameIsDroppedNotCorrupt) {
    const auto framer = make_framer(FramingOptions{FramingKind::delimiter, kFrameDelimiter, 4});
    Frames sink;
    const FeedResult result = feed(*framer, "toolong\x1A{}\x1A", sink);
    EXPECT_EQ(result.dropped, 1u);
    EXPECT_FALSE(result.corrupt);
    EXPECT_EQ(sink.got, std::vector<std::string>{"{}"});
}

TEST(Framer, FactoryHonoursTheKind) {
    const auto length = make_framer(FramingOptions{FramingKind::length, kFrameDelimiter, kMaxFrameBytes});
    EXPECT_FALSE(length->delimiter().has_value());
    EXPECT_EQ(length->overhead(), 4u);
    const auto delimiter = make_framer(FramingOptions{});
    EXPECT_EQ(delimiter->delimiter(), kFrameDelimiter);
}

}  // namespace
}  // namespace mqlc::core
