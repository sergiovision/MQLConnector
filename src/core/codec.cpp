#include "core/codec.h"

#include <span>
#include <utility>

namespace mqlc::core {

std::string describe(const CorrValue& value) {
    if (const auto* number = std::get_if<std::int64_t>(&value)) return std::to_string(*number);
    return "\"" + std::get<std::string>(value) + "\"";
}

JsonCodec::JsonCodec(std::string route_key, std::string correlation_key)
    : route_key_(std::move(route_key)), correlation_key_(std::move(correlation_key)) {
    keys_[kRoute] = route_key_;
    keys_[kId] = "Id";
    keys_[kSym] = "Sym";
    keys_[kCorr] = correlation_key_;
    key_count_ = correlation_key_.empty() ? kKeys - 1 : kKeys;
}

std::optional<CorrValue> JsonCodec::to_corr(const json::Value& value) {
    if (value.kind == json::Kind::number) {
        if (const auto number = json::to_int64(value)) return CorrValue(*number);
        return std::nullopt;
    }
    if (value.kind == json::Kind::string) {
        if (auto text = json::to_string(value)) return CorrValue(std::move(*text));
    }
    return std::nullopt;
}

std::optional<Envelope> JsonCodec::inspect(std::string_view frame) const {
    std::array<json::Value, kKeys> values{};
    if (!json::read_members(frame, std::span<const std::string_view>(keys_.data(), key_count_),
                            std::span<json::Value>(values.data(), key_count_))) {
        return std::nullopt;
    }
    Envelope envelope;
    envelope.route = json::to_int64(values[kRoute]).value_or(0);
    envelope.id = json::to_int64(values[kId]).value_or(0);
    envelope.sym = values[kSym];
    if (key_count_ == kKeys) envelope.corr = to_corr(values[kCorr]);
    return envelope;
}

std::string_view JsonCodec::protect(std::string_view frame, std::optional<char> delimiter,
                                    std::string& storage) const {
    return delimiter ? json::protect_delimiter(frame, *delimiter, storage) : frame;
}

std::int64_t FbeCodec::read_i64_le(const char* bytes) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return static_cast<std::int64_t>(value);
}

std::int32_t FbeCodec::read_i32_le(const char* bytes) noexcept {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return static_cast<std::int32_t>(value);
}

std::optional<Envelope> FbeCodec::inspect(std::string_view frame) const {
    if (frame.size() < kMinFrame) return std::nullopt;
    Envelope envelope;
    envelope.route = read_i64_le(frame.data() + kRouteOffset);
    envelope.id = read_i32_le(frame.data() + kIdOffset);
    if (const std::int64_t corr = read_i64_le(frame.data() + kCorrOffset); corr != 0) envelope.corr = corr;
    return envelope;
}

std::string_view FbeCodec::protect(std::string_view frame, std::optional<char> /*delimiter*/,
                                   std::string& /*storage*/) const {
    return frame;
}

}  // namespace mqlc::core
