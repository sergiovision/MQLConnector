// What the session needs to know about a frame without understanding it: which mailbox it goes
// to (the route key) and, when correlation is on, which request it answers.
//
// JsonCodec serves Protocol "tcp"; FbeCodec serves Protocol "fbe" (binary, length framing).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "core/signal_json.h"

namespace mqlc::core {

/// A correlation value: an integer or a decoded string. Equal when both are integers and equal,
/// or both are strings and equal (7 and "7" differ).
using CorrValue = std::variant<std::int64_t, std::string>;

[[nodiscard]] std::string describe(const CorrValue& value);

/// The routing facts of one frame. `sym` is a slice of the inspected frame.
struct Envelope {
    std::int64_t route = 0;        // missing or not an integer: 0
    std::optional<CorrValue> corr;  // only when correlation is on and the member is present
    std::int64_t id = 0;           // `Id`, for log lines
    json::Value sym;               // `Sym`, for log lines
};

class Codec {
public:
    virtual ~Codec() = default;

    /// nullopt if `frame` is not a well-formed message of this codec.
    [[nodiscard]] virtual std::optional<Envelope> inspect(std::string_view frame) const = 0;

    /// Makes an inspected frame safe to send with a framing that reserves `delimiter`. Returns
    /// `frame` itself (no copy) when nothing needs changing, else the result built in `storage`.
    [[nodiscard]] virtual std::string_view protect(std::string_view frame, std::optional<char> delimiter,
                                                   std::string& storage) const = 0;

    [[nodiscard]] virtual bool correlates() const noexcept = 0;
};

/// JSON objects. Routes by the top-level member `route_key` (an integer or a numeric string);
/// correlates by `correlation_key` when it is not empty. One validating pass per frame reads the
/// route, the correlation value, `Id` and `Sym` together, without allocating unless a string
/// correlation value has to be decoded.
class JsonCodec final : public Codec {
public:
    explicit JsonCodec(std::string route_key = "ChartId", std::string correlation_key = {});

    // The key views point into the members: neither copyable nor movable.
    JsonCodec(const JsonCodec&) = delete;
    JsonCodec& operator=(const JsonCodec&) = delete;

    [[nodiscard]] std::optional<Envelope> inspect(std::string_view frame) const override;
    [[nodiscard]] std::string_view protect(std::string_view frame, std::optional<char> delimiter,
                                           std::string& storage) const override;
    [[nodiscard]] bool correlates() const noexcept override { return key_count_ == kKeys; }

    /// A member value as a correlation value: integers (numbers, truncated like Newtonsoft) and
    /// strings; anything else (null, booleans, containers) is no correlation value.
    [[nodiscard]] static std::optional<CorrValue> to_corr(const json::Value& value);

private:
    static constexpr std::size_t kRoute = 0, kId = 1, kSym = 2, kCorr = 3, kKeys = 4;

    const std::string route_key_;
    const std::string correlation_key_;
    std::array<std::string_view, kKeys> keys_{};
    std::size_t key_count_ = kKeys - 1;  // kKeys when correlating
};

/// Fast Binary Encoding final-model messages (Protocol "fbe", length framing). The framer strips
/// the u32 size, so a frame is `u32 type | Envelope | ...` and the envelope's leading fixed-size
/// members sit at fixed offsets: ChartId (route) i64 at 4, ReqId (correlation) i64 at 12 and Id
/// i32 at 20, all little-endian. Nothing else is decoded: the codec knows no schema.
/// A ReqId of 0 means "uncorrelated".
class FbeCodec final : public Codec {
public:
    static constexpr std::size_t kTypeOffset = 0;
    static constexpr std::size_t kRouteOffset = 4;
    static constexpr std::size_t kCorrOffset = 12;
    static constexpr std::size_t kIdOffset = 20;
    static constexpr std::size_t kMinFrame = 24;  // up to and including Id

    [[nodiscard]] std::optional<Envelope> inspect(std::string_view frame) const override;
    /// Length framing reserves no byte: always `frame` itself.
    [[nodiscard]] std::string_view protect(std::string_view frame, std::optional<char> delimiter,
                                           std::string& storage) const override;
    [[nodiscard]] bool correlates() const noexcept override { return true; }

    [[nodiscard]] static std::int64_t read_i64_le(const char* bytes) noexcept;
    [[nodiscard]] static std::int32_t read_i32_le(const char* bytes) noexcept;
};

}  // namespace mqlc::core
