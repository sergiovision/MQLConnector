#include "core/signal_json.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace mqlc::core::json {
namespace {

std::optional<Value> member(std::string_view doc, std::string_view key) {
    const std::array<std::string_view, 1> keys{key};
    std::array<Value, 1> values{};
    if (!read_members(doc, keys, values)) return std::nullopt;
    return values[0];
}

TEST(SignalJson, ReadsHeader) {
    const auto h = read_signal_header(R"({"Id":1023,"ChartId":131234567890123,"Sym":"EURUSD"})");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 1023);
    EXPECT_EQ(h->chart_id, 131234567890123);
    EXPECT_EQ(to_string(h->sym), "EURUSD");
}

TEST(SignalJson, MissingKeysReadAsZero) {
    const auto h = read_signal_header(R"({"Data":"x"})");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 0);
    EXPECT_EQ(h->chart_id, 0);
    EXPECT_EQ(h->sym.kind, Kind::missing);
}

TEST(SignalJson, NumbersAsStringsAndFloats) {
    auto h = read_signal_header(R"({"Id":"1039","ChartId":"-5"})");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 1039);
    EXPECT_EQ(h->chart_id, -5);
    h = read_signal_header(R"({"Id":1.9e3,"ChartId":12.7})");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 1900);
    EXPECT_EQ(h->chart_id, 12);
    EXPECT_FALSE(to_int64(*member(R"({"x":"abc"})", "x")));
    EXPECT_FALSE(to_int64(*member(R"({"x":1e300})", "x")));
}

TEST(SignalJson, SkipsArbitrarilyNestedData) {
    const std::string doc = R"(  {  "Data" : { "a" : [ 1 , { "b" : [ [ ] , { } , "}]" ] } ] , "c" : null } ,
        "Id" : 1039 , "ChartId" : 7 }  )";
    const auto h = read_signal_header(doc);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 1039);
    EXPECT_EQ(h->chart_id, 7);
    const auto data = member(doc, "Data");
    ASSERT_TRUE(data);
    EXPECT_EQ(data->kind, Kind::object);
    EXPECT_EQ(payload_text(*data).front(), '{');
    EXPECT_EQ(payload_text(*data).back(), '}');
}

TEST(SignalJson, DeepNestingWithinLimit) {
    std::string deep(1000, '[');
    deep += std::string(1000, ']');
    EXPECT_TRUE(read_signal_header("{\"Data\":" + deep + ",\"Id\":1}"));
    std::string too_deep(kMaxDepth + 1, '[');
    too_deep += std::string(kMaxDepth + 1, ']');
    EXPECT_FALSE(read_signal_header("{\"Data\":" + too_deep + "}"));
}

TEST(SignalJson, StringEscapes) {
    // JSON's backslash-u escapes are written in ordinary literals (doubled backslash), never inside
    // raw ones: MSVC 19.4x translates universal-character-names even in raw string literals,
    // rejecting surrogates and control characters (C3850) and decoding the rest itself.
    const auto v = member(R"({"s":"a\"b\\c\/d\n\t)" "\\u00e9\\ud83d\\ude00\"}", "s");
    ASSERT_TRUE(v);
    EXPECT_EQ(to_string(*v), "a\"b\\c/d\n\t\xC3\xA9\xF0\x9F\x98\x80");
    // A lone surrogate escape decodes to U+FFFD.
    EXPECT_EQ(to_string(*member("{\"s\":\"\\ud800x\"}", "s")), "\xEF\xBF\xBDx");
}

TEST(SignalJson, EscapedKeysMatch) {
    const auto h = read_signal_header("{\"\\u0049d\":5}");
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 5);
}

TEST(SignalJson, LastDuplicateWins) {
    EXPECT_EQ(read_signal_header(R"({"Id":1,"Id":2})")->id, 2);
}

TEST(SignalJson, PayloadText) {
    EXPECT_EQ(payload_text(*member(R"({"Data":"1.1;2.2"})", "Data")), "1.1;2.2");
    EXPECT_EQ(payload_text(*member(R"({"Data":[1,2]})", "Data")), "[1,2]");
    EXPECT_EQ(payload_text(*member(R"({"Data":null})", "Data")), "");
    EXPECT_EQ(payload_text(*member(R"({"Id":1})", "Data")), "");
    EXPECT_EQ(payload_text(*member(R"({"Data":42})", "Data")), "42");
}

TEST(SignalJson, RejectsGarbage) {
    for (const char* bad : {"", "   ", "[]", "\"x\"", "{", "}", "{\"Id\"}", "{\"Id\":}", "{\"Id\":1,}",
                            "{\"Id\":01}", "{\"Id\":1.}", "{\"Id\":-}", "{\"Id\":tru}", "{\"a\":\"\\x\"}",
                            "{\"a\":\"\\u12\"}", "{\"a\":\"open}", "{\"a\":[1,]}", "{\"a\":[1}",
                            "{\"a\":{\"b\":1]}", "{} {}", "{\"a\":1} x", "{'a':1}"}) {
        EXPECT_FALSE(read_signal_header(bad)) << bad;
    }
}

TEST(SignalJson, AcceptsRawControlCharactersInStrings) {
    // Newtonsoft does; MQL serializers are not always strict.
    EXPECT_TRUE(read_signal_header("{\"Data\":\"line1\nline2\"}"));
}

TEST(SignalJson, WriterEscapes) {
    const std::string out = ObjectWriter()
                                .add_int("Id", -1)
                                .add_string("S", "q\"b\\n\n\x1A\x01\xC3\xA9")
                                .finish();
    EXPECT_EQ(out, R"({"Id":-1,"S":"q\"b\\n\n)" "\\u001a\\u0001\xC3\xA9\"}");
    EXPECT_EQ(out.find('\x1A'), std::string::npos);
    const auto round = member(out, "S");
    ASSERT_TRUE(round);
    EXPECT_EQ(to_string(*round), "q\"b\\n\n\x1A\x01\xC3\xA9");
}

TEST(SignalJson, WriterEmptyObject) { EXPECT_EQ(ObjectWriter().finish(), "{}"); }

}  // namespace
}  // namespace mqlc::core::json
