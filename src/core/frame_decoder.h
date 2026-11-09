// Streaming splitter for FinCore's wire framing: one UTF-8 JSON document per frame, terminated by
// the byte 0x1A (ASCII SUB), as the server's messaging endpoint frames it.
//
// TCP preserves no boundaries, so a frame may arrive split over many reads and one read may carry
// several frames. 0x1A never occurs inside a UTF-8 multi-byte sequence, so splitting raw bytes is
// safe even when a read boundary cuts a character in half.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mqlc::core {

inline constexpr char kFrameDelimiter = '\x1A';
inline constexpr std::size_t kMaxFrameBytes = 16u * 1024u * 1024u;

class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_frame_bytes = kMaxFrameBytes) noexcept
        : max_frame_bytes_(max_frame_bytes) {}

    /// Feeds one read's bytes. Calls `on_frame(std::string_view)` for every completed frame, in
    /// order; the view is valid only during the call. A frame that grows past the cap without a
    /// delimiter is dropped, and so is the rest of it up to the next delimiter, so the stream
    /// resynchronises on the following frame. Returns the number of frames dropped by this call.
    template <class OnFrame>
    std::size_t feed(std::span<const char> bytes, OnFrame&& on_frame) {
        std::size_t dropped = 0;
        std::string_view rest(bytes.data(), bytes.size());
        while (!rest.empty()) {
            const std::size_t end = rest.find(kFrameDelimiter);
            const bool complete = end != std::string_view::npos;
            const std::string_view chunk = rest.substr(0, complete ? end : rest.size());
            rest.remove_prefix(complete ? end + 1 : rest.size());

            if (discarding_) {
                discarding_ = !complete;
                continue;
            }
            if (pending_.size() + chunk.size() > max_frame_bytes_) {
                pending_.clear();
                discarding_ = !complete;
                ++dropped;
                continue;
            }
            if (!complete) {
                pending_.append(chunk);
            } else if (pending_.empty()) {
                on_frame(chunk);  // whole frame inside this read: no copy
            } else {
                pending_.append(chunk);
                on_frame(std::string_view(pending_));
                pending_.clear();
            }
        }
        dropped_total_ += dropped;
        return dropped;
    }

    /// Forgets a partial frame, e.g. after a disconnect: it can never complete.
    void reset() noexcept {
        pending_.clear();
        discarding_ = false;
    }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_total_; }

private:
    std::string pending_;  // bytes of the frame being assembled (capacity is kept between frames)
    std::size_t max_frame_bytes_;
    std::uint64_t dropped_total_ = 0;
    bool discarding_ = false;  // inside an oversized frame: skip to the next delimiter
};

}  // namespace mqlc::core
