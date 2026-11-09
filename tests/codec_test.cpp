// JsonCodec (route key, correlation values) and the delimiter protection of outgoing JSON.
#include "core/codec.h"
#include "core/signal_json.h"

#include <gtest/gtest.h>

#include <string>

namespace mqlc::core {
namespace {

/// The JSON escape for code unit `hex4`, built at run time: a backslash-u spelled in a literal
/// trips MSVC (see signal_json_test.cpp).
std::string json_escape(const char* hex4) { return std::string(1, '\\') + "u" + hex4; }

TEST(JsonCodec, DefaultRoutesByChartId) {
    const JsonCodec codec;
    EXPECT_FALSE(codec.correlates());
    const auto e = codec.inspect(R"({"Id":1001,"ChartId":"77","Sym":"EURUSD","ReqId":5})");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->route, 77);
    EXPECT_EQ(e->id, 1001);
    EXPECT_EQ(json::to_string(e->sym), "EURUSD");
    EXPECT_FALSE(e->corr);  // correlation off: never read, never allocated
    EXPECT_EQ(codec.inspect(R"({"Id":1})")->route, 0);
    EXPECT_FALSE(codec.inspect("[1]"));
    EXPECT_FALSE(codec.inspect("{\"Id\":"));
}

TEST(JsonCodec, CustomRouteKey) {
    const JsonCodec codec("Client");
    EXPECT_EQ(codec.inspect(R"({"Client":42,"ChartId":7})")->route, 42);
    EXPECT_EQ(codec.inspect(R"({"Client":"-3"})")->route, -3);
    EXPECT_EQ(codec.inspect(R"({"ChartId":7})")->route, 0);       // missing: route 0
    EXPECT_EQ(codec.inspect(R"({"Client":"abc"})")->route, 0);    // not an integer: route 0
    EXPECT_EQ(codec.inspect(R"({"Client":{"x":1}})")->route, 0);
}

TEST(JsonCodec, CorrelationValues) {
    const JsonCodec codec("ChartId", "ReqId");
    EXPECT_TRUE(codec.correlates());
    EXPECT_EQ(codec.inspect(R"({"ReqId":7})")->corr, CorrValue(std::int64_t{7}));
    EXPECT_EQ(codec.inspect(R"({"ReqId":"a-1"})")->corr, CorrValue(std::string("a-1")));
    EXPECT_EQ(codec.inspect("{\"ReqId\":\"" + json_escape("0041") + "\"}")->corr, CorrValue(std::string("A")));
    EXPECT_FALSE(codec.inspect(R"({"ChartId":1})")->corr);
    EXPECT_FALSE(codec.inspect(R"({"ReqId":null})")->corr);
    EXPECT_FALSE(codec.inspect(R"({"ReqId":true})")->corr);
    EXPECT_FALSE(codec.inspect(R"({"ReqId":[7]})")->corr);
}

TEST(JsonCodec, CorrelationEquality) {
    const JsonCodec codec("ChartId", "ReqId");
    const auto number = codec.inspect(R"({"ReqId":7})")->corr;
    const auto text = codec.inspect(R"({"ReqId":"7"})")->corr;
    EXPECT_NE(number, text);  // an integer never equals a string
    EXPECT_EQ(number, codec.inspect(R"({"ReqId":7.0})")->corr);
    EXPECT_EQ(text, codec.inspect(R"({"ReqId":"7"})")->corr);
    EXPECT_EQ(describe(*number), "7");
    EXPECT_EQ(describe(*text), "\"7\"");
}

TEST(JsonCodec, CorrelationKeyMayEqualTheRouteKey) {
    const JsonCodec codec("ChartId", "ChartId");
    const auto e = codec.inspect(R"({"ChartId":9})");
    EXPECT_EQ(e->route, 9);
    EXPECT_EQ(e->corr, CorrValue(std::int64_t{9}));
}

TEST(ProtectDelimiter, NoDelimiterMeansNoCopy) {
    const std::string doc = R"({"Id":1,"Data":"plain"})";
    std::string storage;
    const auto out = json::protect_delimiter(doc, '\x1A', storage);
    EXPECT_EQ(out.data(), doc.data());
    EXPECT_TRUE(storage.empty());
}

TEST(ProtectDelimiter, SubInsideStringsIsEscaped) {
    std::string storage;
    EXPECT_EQ(json::protect_delimiter("{\"a\x1A\":\"x\x1A\x1Ay\"}", '\x1A', storage),
              "{\"a" + json_escape("001a") + "\":\"x" + json_escape("001a") + json_escape("001a") + "y\"}");
}

TEST(ProtectDelimiter, NewlineDelimiterWithPrettyPrintedJson) {
    // Raw newlines between tokens (pretty-printing) and inside a string (accepted, as Newtonsoft
    // does). With Delimiter=10 the former become spaces, the latter an escape.
    const std::string doc = "{\n  \"Id\": 1,\n  \"Data\": \"line1\nline2\",\n  \"Q\": \"a\\\"\n\\\\\"\n}\n";
    ASSERT_TRUE(JsonCodec().inspect(doc));
    std::string storage;
    const std::string out(json::protect_delimiter(doc, '\n', storage));
    EXPECT_EQ(out.find('\n'), std::string::npos);
    EXPECT_EQ(out, "{   \"Id\": 1,   \"Data\": \"line1" + json_escape("000a") + "line2\",   \"Q\": \"a\\\"" +
                       json_escape("000a") + "\\\\\" } ");
    const auto e = JsonCodec().inspect(out);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->id, 1);
    std::array<json::Value, 2> values{};
    const std::array<std::string_view, 2> keys{"Data", "Q"};
    ASSERT_TRUE(json::read_members(out, keys, values));
    EXPECT_EQ(json::to_string(values[0]), "line1\nline2");
    EXPECT_EQ(json::to_string(values[1]), "a\"\n\\");
}

TEST(ProtectDelimiter, TabAndCarriageReturnDelimiters) {
    std::string storage;
    EXPECT_EQ(json::protect_delimiter("{\t\"a\":\"\t\"}", '\t', storage), "{ \"a\":\"" + json_escape("0009") + "\"}");
    EXPECT_EQ(json::protect_delimiter("{\"a\":1}\r", '\r', storage), "{\"a\":1} ");
}

TEST(ProtectDelimiter, CodecProtectsOnlyForDelimiterFraming) {
    const JsonCodec codec;
    const std::string doc = "{\"a\":\"x\x1Ay\"}";
    std::string storage;
    EXPECT_EQ(codec.protect(doc, std::nullopt, storage).data(), doc.data());  // length framing
    EXPECT_EQ(codec.protect(doc, '\x1A', storage), "{\"a\":\"x" + json_escape("001a") + "y\"}");
}

}  // namespace
}  // namespace mqlc::core
