#include "core/framer.h"

#include <algorithm>
#include <limits>

namespace mqlc::core {

bool DelimiterFramer::encode(std::string_view payload, std::string& out) const {
    if (payload.find(delimiter_) != std::string_view::npos) return false;
    out.append(payload);
    out.push_back(delimiter_);
    return true;
}

void LengthPrefixFramer::append_u32_le(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFFu));
}

std::uint32_t LengthPrefixFramer::read_u32_le(const char* bytes) noexcept {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return value;
}

bool LengthPrefixFramer::encode(std::string_view payload, std::string& out) const {
    constexpr std::size_t kMaxPayload = std::numeric_limits<std::uint32_t>::max() - kLengthPrefixBytes;
    if (payload.size() > kMaxPayload) return false;
    append_u32_le(out, static_cast<std::uint32_t>(payload.size() + kLengthPrefixBytes));
    out.append(payload);
    return true;
}

void LengthPrefixFramer::reset() noexcept {
    pending_.clear();
    body_size_ = 0;
    header_used_ = 0;
    have_size_ = false;
}

FeedResult LengthPrefixFramer::feed(std::span<const char> bytes, FrameSink& sink) {
    FeedResult result;
    std::string_view rest(bytes.data(), bytes.size());
    while (true) {
        if (!have_size_) {
            if (rest.empty()) break;
            // The prefix may itself be split across reads.
            const std::size_t take = std::min(kLengthPrefixBytes - header_used_, rest.size());
            std::copy_n(rest.data(), take, header_ + header_used_);
            header_used_ += take;
            rest.remove_prefix(take);
            if (header_used_ < kLengthPrefixBytes) break;
            header_used_ = 0;
            const std::uint32_t total = read_u32_le(header_);
            if (total < kLengthPrefixBytes || total - kLengthPrefixBytes > max_frame_bytes_) {
                reset();
                result.corrupt = true;
                return result;
            }
            body_size_ = total - kLengthPrefixBytes;
            have_size_ = true;
        }
        if (pending_.empty() && rest.size() >= body_size_) {
            sink.on_frame(rest.substr(0, body_size_));  // whole payload inside this read: no copy
            rest.remove_prefix(body_size_);
            have_size_ = false;
            continue;
        }
        const std::size_t take = std::min(body_size_ - pending_.size(), rest.size());
        pending_.append(rest.data(), take);
        rest.remove_prefix(take);
        if (pending_.size() < body_size_) break;  // rest is empty: wait for the next read
        sink.on_frame(std::string_view(pending_));
        pending_.clear();
        have_size_ = false;
    }
    return result;
}

std::unique_ptr<Framer> make_framer(const FramingOptions& options) {
    if (options.kind == FramingKind::length) return std::make_unique<LengthPrefixFramer>(options.max_frame_bytes);
    return std::make_unique<DelimiterFramer>(options.max_frame_bytes, options.delimiter);
}

}  // namespace mqlc::core
