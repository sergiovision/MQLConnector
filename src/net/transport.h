// The connection to FinCore as the session sees it: frames in, frames out, connected or not.
// An interface so the session can be tested against a fake and the TCP details stay in one place.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "core/frame_decoder.h"

namespace mqlc::net {

using LogFn = std::function<void(std::string_view)>;

class Transport {
public:
    /// Called on the transport's reader thread with each complete frame (delimiter stripped).
    using FrameHandler = std::function<void(std::string_view frame)>;

    virtual ~Transport() = default;

    /// Waits until connected, up to `timeout`. Cuts short any reconnect backoff in progress.
    [[nodiscard]] virtual bool connect(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    /// Queues one frame for the writer thread and returns at once; the delimiter is appended
    /// here. False (frame dropped) when disconnected, the queue is full, or `frame` contains the
    /// delimiter itself.
    [[nodiscard]] virtual bool send(std::string_view frame) = 0;

    /// Stops and joins the transport's threads. Idempotent. Bounded: every blocking call the
    /// threads make is interruptible (socket shutdown, sliced connect, stoppable waits).
    virtual void stop() = 0;
};

struct TcpOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 2022;
    std::chrono::milliseconds connect_attempt_timeout{1000};
    std::chrono::milliseconds initial_backoff{100};  // the .NET client waited 100 ms, too
    std::chrono::milliseconds max_backoff{2000};
    std::size_t max_queued_bytes = 64u * 1024u * 1024u;
    std::size_t max_frame_bytes = core::kMaxFrameBytes;
};

/// A TCP client that connects in the background and keeps reconnecting until stopped: one
/// connect/reader thread and one writer thread.
[[nodiscard]] std::unique_ptr<Transport> make_tcp_transport(TcpOptions options,
                                                            Transport::FrameHandler on_frame,
                                                            LogFn log);

}  // namespace mqlc::net
