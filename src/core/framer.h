// Wire framing: how frames are cut out of, and put into, a TCP byte stream.
//
// Two framings exist, chosen once per connection (InitLib key `Framing`):
//
//   delimiter  payload, then one delimiter byte (default 0x1A, FinCore's). Any byte 1..31 may be
//              chosen with `Delimiter`; the payload must not contain it (Codec::protect sees to
//              that). Splitting raw bytes is safe even when a read cuts a UTF-8 character in half:
//              no byte below 0x80 occurs inside a multi-byte sequence.
//   length     a u32 little-endian total length that counts its own 4 bytes, then the payload:
//              the frame layout of FastBinaryEncoding's protocol. Any payload is allowed.
//
// The 16 MiB cap applies to the payload in both framings. A delimiter stream drops an oversized
// frame and resynchronises on the next delimiter; a length stream cannot be resynchronised
// in-band (any byte may be a length), so a bad prefix reports `corrupt` and the transport closes
// and reconnects.
//
// Per-byte work happens inside each implementation (memchr for the delimiter); the virtual calls
// are one per read (`feed`), one per frame (`FrameSink::on_frame`) and one per sent frame
// (`encode`).
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mqlc::core {

inline constexpr char kFrameDelimiter = '\x1A';
inline constexpr std::size_t kMaxFrameBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kLengthPrefixBytes = 4;

enum class FramingKind : std::uint8_t { delimiter, length };

struct FramingOptions {
    FramingKind kind = FramingKind::delimiter;
    char delimiter = kFrameDelimiter;              // delimiter framing only: a byte in 1..31
    std::size_t max_frame_bytes = kMaxFrameBytes;  // payload cap, both framings
};

/// Receives the frames a Framer cuts out. The view is valid only during the call: it points into
/// the read buffer when the frame arrived in one read, into the framer's own buffer otherwise.
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void on_frame(std::string_view frame) = 0;
};

/// Adapts any callable to a FrameSink without allocating.
template <class F>
class CallbackSink final : public FrameSink {
public:
    explicit CallbackSink(F& callback) noexcept : callback_(callback) {}
    void on_frame(std::string_view frame) override { callback_(frame); }

private:
    F& callback_;
};

struct FeedResult {
    std::size_t dropped = 0;  // oversized frames skipped (the stream resynchronised by itself)
    bool corrupt = false;     // the stream cannot be resynchronised: close the connection
};

class Framer {
public:
    virtual ~Framer() = default;

    /// Feeds one read's bytes; calls `sink` for every completed frame, in order. After a result
    /// with `corrupt` set, the framer has discarded its state and must not be fed the rest of the
    /// same connection's stream.
    virtual FeedResult feed(std::span<const char> bytes, FrameSink& sink) = 0;

    /// Appends `payload` framed to `out`. False if it cannot be framed (it contains the
    /// delimiter, or it is too long for a u32 length). Thread-safe: depends on settings only.
    [[nodiscard]] virtual bool encode(std::string_view payload, std::string& out) const = 0;

    /// Forgets a partial frame, e.g. after a disconnect: it can never complete.
    virtual void reset() noexcept = 0;

    /// Bytes `encode` adds to a payload.
    [[nodiscard]] virtual std::size_t overhead() const noexcept = 0;
    /// The delimiter byte payloads must not contain, if this framing has one.
    [[nodiscard]] virtual std::optional<char> delimiter() const noexcept = 0;
};

/// Delimiter framing. Also usable without virtual calls through the template `feed` (the old
/// FrameDecoder interface, kept for the loopback tools and tests).
class DelimiterFramer final : public Framer {
public:
    explicit DelimiterFramer(std::size_t max_frame_bytes = kMaxFrameBytes,
                             char delimiter = kFrameDelimiter) noexcept
        : max_frame_bytes_(max_frame_bytes), delimiter_(delimiter) {}

    /// Calls `on_frame(std::string_view)` for every completed frame. A frame that grows past the
    /// cap without a delimiter is dropped, and so is the rest of it up to the next delimiter, so
    /// the stream resynchronises on the following frame. Returns the frames dropped by this call.
    template <class OnFrame>
        requires std::invocable<OnFrame&, std::string_view>
    std::size_t feed(std::span<const char> bytes, OnFrame&& on_frame) {
        std::size_t dropped = 0;
        std::string_view rest(bytes.data(), bytes.size());
        while (!rest.empty()) {
            const std::size_t end = rest.find(delimiter_);
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

    FeedResult feed(std::span<const char> bytes, FrameSink& sink) override {
        return FeedResult{feed(bytes, [&sink](std::string_view frame) { sink.on_frame(frame); }), false};
    }

    [[nodiscard]] bool encode(std::string_view payload, std::string& out) const override;

    void reset() noexcept override {
        pending_.clear();
        discarding_ = false;
    }

    [[nodiscard]] std::size_t overhead() const noexcept override { return 1; }
    [[nodiscard]] std::optional<char> delimiter() const noexcept override { return delimiter_; }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_total_; }

private:
    std::string pending_;  // bytes of the frame being assembled (capacity is kept between frames)
    std::size_t max_frame_bytes_;
    std::uint64_t dropped_total_ = 0;
    char delimiter_;
    bool discarding_ = false;  // inside an oversized frame: skip to the next delimiter
};

/// Length-prefix framing: [u32 LE total length, including these 4 bytes][payload].
class LengthPrefixFramer final : public Framer {
public:
    explicit LengthPrefixFramer(std::size_t max_frame_bytes = kMaxFrameBytes) noexcept
        : max_frame_bytes_(max_frame_bytes) {}

    /// A prefix below 4 or announcing a payload above the cap makes the stream unreadable:
    /// everything buffered is dropped and `corrupt` is reported. A length stream has no marker to
    /// resynchronise on, so the only recovery is a new connection.
    FeedResult feed(std::span<const char> bytes, FrameSink& sink) override;
    [[nodiscard]] bool encode(std::string_view payload, std::string& out) const override;
    void reset() noexcept override;

    [[nodiscard]] std::size_t overhead() const noexcept override { return kLengthPrefixBytes; }
    [[nodiscard]] std::optional<char> delimiter() const noexcept override { return std::nullopt; }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return header_used_ + pending_.size(); }

    /// Explicit little-endian conversions: no memcpy of a host integer, so big-endian hosts agree.
    static void append_u32_le(std::string& out, std::uint32_t value);
    [[nodiscard]] static std::uint32_t read_u32_le(const char* bytes) noexcept;

private:
    std::string pending_;  // payload bytes of the frame being assembled
    std::size_t max_frame_bytes_;
    std::size_t body_size_ = 0;  // payload size announced by the current prefix
    std::size_t header_used_ = 0;
    char header_[kLengthPrefixBytes] = {};
    bool have_size_ = false;  // the current frame's prefix is complete
};

[[nodiscard]] std::unique_ptr<Framer> make_framer(const FramingOptions& options);

}  // namespace mqlc::core
