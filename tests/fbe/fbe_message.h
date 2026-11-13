// Test-only builder/reader of tests/fbe/test.fbe `TestMessage` in FBE final format:
//   u32 size (counting itself) | u32 type | Envelope{i64 ChartId, i64 ReqId, i32 Id, i64 Flags,
//   i64 ObjectId, i32 Value, string Sym, string Key} | string Payload
// Strings are u32 byte length + UTF-8, everything little-endian.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mqlc::test::fbe {

inline constexpr std::uint32_t kTestMessageType = 1;

struct TestMessage {
    std::int64_t chart_id = 0;
    std::int64_t req_id = 0;
    std::int32_t id = 0;
    std::int64_t flags = 0;
    std::int64_t object_id = 0;
    std::int32_t value = 0;
    std::string sym;
    std::string key;
    std::string payload;
};

inline void put_le(std::string& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

inline std::uint64_t get_le(std::string_view in, std::size_t offset, int bytes) {
    std::uint64_t value = 0;
    for (int i = bytes - 1; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(in[offset + i]);
    return value;
}

inline void put_string(std::string& out, std::string_view text) {
    put_le(out, text.size(), 4);
    out.append(text);
}

/// The whole message, its u32 size included (what MQL hands to DoSendBinary).
inline std::string encode(const TestMessage& m) {
    std::string out(4, '\0');
    put_le(out, kTestMessageType, 4);
    put_le(out, static_cast<std::uint64_t>(m.chart_id), 8);
    put_le(out, static_cast<std::uint64_t>(m.req_id), 8);
    put_le(out, static_cast<std::uint32_t>(m.id), 4);
    put_le(out, static_cast<std::uint64_t>(m.flags), 8);
    put_le(out, static_cast<std::uint64_t>(m.object_id), 8);
    put_le(out, static_cast<std::uint32_t>(m.value), 4);
    put_string(out, m.sym);
    put_string(out, m.key);
    put_string(out, m.payload);
    const auto size = static_cast<std::uint32_t>(out.size());
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((size >> (8 * i)) & 0xFFu);
    return out;
}

/// A whole message (size included) back into a TestMessage; nullopt when malformed.
inline std::optional<TestMessage> decode(std::string_view in) {
    std::size_t at = 0;
    const auto need = [&](std::size_t n) { return at + n <= in.size(); };
    if (!need(8) || get_le(in, 0, 4) != in.size() || get_le(in, 4, 4) != kTestMessageType) return std::nullopt;
    at = 8;
    if (!need(48)) return std::nullopt;
    TestMessage m;
    m.chart_id = static_cast<std::int64_t>(get_le(in, at, 8));
    m.req_id = static_cast<std::int64_t>(get_le(in, at + 8, 8));
    m.id = static_cast<std::int32_t>(get_le(in, at + 16, 4));
    m.flags = static_cast<std::int64_t>(get_le(in, at + 20, 8));
    m.object_id = static_cast<std::int64_t>(get_le(in, at + 28, 8));
    m.value = static_cast<std::int32_t>(get_le(in, at + 36, 4));
    at += 40;
    for (std::string* text : {&m.sym, &m.key, &m.payload}) {
        if (!need(4)) return std::nullopt;
        const auto length = static_cast<std::size_t>(get_le(in, at, 4));
        at += 4;
        if (!need(length)) return std::nullopt;
        text->assign(in.substr(at, length));
        at += length;
    }
    return at == in.size() ? std::optional<TestMessage>(std::move(m)) : std::nullopt;
}

/// A received frame (no u32 size, as the framer delivers it) back into a whole message.
inline std::string reframe(std::string_view frame) {
    std::string out;
    put_le(out, frame.size() + 4, 4);
    out.append(frame);
    return out;
}

}  // namespace mqlc::test::fbe
