#include "core/utf.h"

#include <gtest/gtest.h>

#include <string>

namespace mqlc::utf {
namespace {

const std::string kReplacementUtf8 = "\xEF\xBF\xBD";

TEST(Utf, AsciiAndBmpRoundTrip) {
    const std::u16string text = u"EURUSD \u00e9\u20ac\u4e2d";
    const std::string utf8 = to_utf8(std::u16string_view(text));
    EXPECT_EQ(utf8, "EURUSD \xC3\xA9\xE2\x82\xAC\xE4\xB8\xAD");
    EXPECT_EQ(to_utf16<char16_t>(utf8), text);
}

TEST(Utf, SurrogatePairs) {
    const std::u16string emoji = u"\U0001F600";  // D83D DE00
    ASSERT_EQ(emoji.size(), 2u);
    EXPECT_EQ(to_utf8(std::u16string_view(emoji)), "\xF0\x9F\x98\x80");
    EXPECT_EQ(to_utf16<char16_t>("\xF0\x9F\x98\x80"), emoji);
}

TEST(Utf, LoneSurrogatesBecomeReplacement) {
    const std::u16string lone_high{char16_t{0xD83D}, u'a'};
    EXPECT_EQ(to_utf8(std::u16string_view(lone_high)), kReplacementUtf8 + "a");
    const std::u16string lone_low{u'a', char16_t{0xDE00}};
    EXPECT_EQ(to_utf8(std::u16string_view(lone_low)), "a" + kReplacementUtf8);
    const std::u16string trailing_high{char16_t{0xD83D}};
    EXPECT_EQ(to_utf8(std::u16string_view(trailing_high)), kReplacementUtf8);
}

TEST(Utf, InvalidUtf8BecomesReplacement) {
    const std::u16string r{char16_t{0xFFFD}};
    EXPECT_EQ(to_utf16<char16_t>("\x80"), r);                           // stray continuation
    EXPECT_EQ(to_utf16<char16_t>("\xFF"), r);                           // impossible lead
    EXPECT_EQ(to_utf16<char16_t>("\xC0\xAF"), r);                       // overlong '/'
    EXPECT_EQ(to_utf16<char16_t>("\xED\xA0\x80"), r);                   // encoded surrogate
    EXPECT_EQ(to_utf16<char16_t>("\xF4\x90\x80\x80"), r);               // > U+10FFFF
    EXPECT_EQ(to_utf16<char16_t>("\xE2\x82"), r);                       // truncated at end
    EXPECT_EQ(to_utf16<char16_t>("\xE2\x82" "A"), r + u"A");            // interrupted
}

TEST(Utf, EmptyInput) {
    EXPECT_TRUE(to_utf8(std::u16string_view()).empty());
    EXPECT_TRUE(to_utf16<char16_t>("").empty());
}

#ifdef _WIN32
TEST(Utf, WideCharOnWindows) {
    EXPECT_EQ(to_utf8(std::wstring_view(L"\u20ac")), "\xE2\x82\xAC");
    EXPECT_EQ(to_utf16<wchar_t>("\xE2\x82\xAC"), L"\u20ac");
}
#endif

}  // namespace
}  // namespace mqlc::utf
