// UTF-16 <-> UTF-8 conversion for the MQL string boundary.
//
// MQL5 passes `string` to a DLL as UTF-16 (`const wchar_t*` on Windows); the wire is UTF-8.
// Both directions replace malformed input with U+FFFD instead of failing: a stray lone surrogate
// in a chart comment must not cost the whole message. Header-only so it works for `wchar_t`
// (Windows ABI) and `char16_t` (portable tests) alike.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

namespace mqlc::utf {

template <class C>
concept Utf16Unit = (std::same_as<C, char16_t> || std::same_as<C, wchar_t>) && sizeof(C) == 2;

inline constexpr char32_t kReplacement = 0xFFFD;

namespace detail {

constexpr bool is_high_surrogate(char32_t u) noexcept { return u >= 0xD800 && u <= 0xDBFF; }
constexpr bool is_low_surrogate(char32_t u) noexcept { return u >= 0xDC00 && u <= 0xDFFF; }

inline void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

template <Utf16Unit C>
void append_utf16(std::basic_string<C>& out, char32_t cp) {
    if (cp < 0x10000) {
        out.push_back(static_cast<C>(cp));
    } else {
        const char32_t v = cp - 0x10000;
        out.push_back(static_cast<C>(0xD800 + (v >> 10)));
        out.push_back(static_cast<C>(0xDC00 + (v & 0x3FF)));
    }
}

}  // namespace detail

/// Appends the UTF-8 encoding of `in` to `out`. Unpaired surrogates become U+FFFD.
template <Utf16Unit C>
void append_utf8(std::basic_string_view<C> in, std::string& out) {
    out.reserve(out.size() + in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(in[i]));
        if (detail::is_high_surrogate(unit) && i + 1 < in.size()) {
            const auto next = static_cast<char32_t>(static_cast<std::uint16_t>(in[i + 1]));
            if (detail::is_low_surrogate(next)) {
                detail::append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++i;
                continue;
            }
        }
        const bool lone = detail::is_high_surrogate(unit) || detail::is_low_surrogate(unit);
        detail::append_utf8(out, lone ? kReplacement : unit);
    }
}

/// Appends the UTF-16 encoding of `in` to `out`. Every malformed sequence (bad lead byte,
/// truncated or interrupted sequence, overlong form, encoded surrogate, > U+10FFFF) becomes one
/// U+FFFD and decoding resumes at the first byte that could not belong to it.
template <Utf16Unit C>
void append_utf16(std::string_view in, std::basic_string<C>& out) {
    out.reserve(out.size() + in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto lead = static_cast<unsigned char>(in[i]);
        if (lead < 0x80) {
            out.push_back(static_cast<C>(lead));
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2, cp = lead & 0x1Fu, minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, cp = lead & 0x0Fu, minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, cp = lead & 0x07u, minimum = 0x10000;
        } else {
            detail::append_utf16(out, kReplacement);  // continuation byte or 0xF8..0xFF as lead
            ++i;
            continue;
        }
        std::size_t taken = 1;
        while (taken < length && i + taken < in.size()) {
            const auto next = static_cast<unsigned char>(in[i + taken]);
            if ((next & 0xC0) != 0x80) break;
            cp = (cp << 6) | (next & 0x3Fu);
            ++taken;
        }
        i += taken;
        const bool valid = taken == length && cp >= minimum && cp <= 0x10FFFF &&
                           !(cp >= 0xD800 && cp <= 0xDFFF);
        detail::append_utf16(out, valid ? cp : kReplacement);
    }
}

template <Utf16Unit C>
[[nodiscard]] std::string to_utf8(std::basic_string_view<C> in) {
    std::string out;
    append_utf8(in, out);
    return out;
}

template <Utf16Unit C>
[[nodiscard]] std::basic_string<C> to_utf16(std::string_view in) {
    std::basic_string<C> out;
    append_utf16(in, out);
    return out;
}

}  // namespace mqlc::utf
