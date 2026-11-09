// Thin RAII wrappers over the OS socket API. This is the only platform-specific network code:
// socket_winsock.cpp implements it for Windows, socket_posix.cpp for macOS/Linux, and everything
// above (reconnects, framing, threads) is shared.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>

namespace mqlc::net {

using NativeSocket = std::uintptr_t;  // SOCKET on Windows, int on POSIX
inline constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};

/// Keeps the socket library initialised (WSAStartup/WSACleanup) while an instance lives.
class NetworkScope {
public:
    NetworkScope() noexcept;
    ~NetworkScope();
    NetworkScope(const NetworkScope&) = delete;
    NetworkScope& operator=(const NetworkScope&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

/// A connected TCP stream socket, move-only. Blocking I/O; `shutdown()` may be called from
/// another thread to break a pending `receive`/`send_all`, which is how threads are stopped.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(NativeSocket handle) noexcept : handle_(handle) {}
    ~Socket() { close(); }

    Socket(Socket&& other) noexcept : handle_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.release();
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// Resolves `host` and connects to the first address that accepts within `timeout`, giving
    /// up early when `stop` is requested. The result has TCP_NODELAY set: requests are small and
    /// latency-bound, and Nagle plus delayed ACK would add tens of milliseconds per round trip.
    /// On failure returns an invalid socket and describes why in `error`.
    [[nodiscard]] static Socket connect(const std::string& host, std::uint16_t port,
                                        std::chrono::milliseconds timeout, std::stop_token stop,
                                        std::string& error);

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }

    /// Bytes read (> 0), 0 on orderly close, < 0 on error.
    [[nodiscard]] std::ptrdiff_t receive(std::span<char> buffer) noexcept;
    [[nodiscard]] bool send_all(std::span<const char> bytes) noexcept;

    /// Thread-safe: ends both directions, waking any thread blocked on this socket.
    void shutdown() noexcept;
    /// Closes with RST instead of FIN (no TIME_WAIT); used by test servers to simulate a crash.
    void abort() noexcept;
    void close() noexcept;

private:
    friend class Listener;
    NativeSocket release() noexcept {
        const NativeSocket h = handle_;
        handle_ = kInvalidSocket;
        return h;
    }

    NativeSocket handle_ = kInvalidSocket;
};

/// A loopback listening socket for the test/bench echo server.
class Listener {
public:
    /// Binds 127.0.0.1:`port` (0 = any free port).
    [[nodiscard]] static Listener bind_loopback(std::uint16_t port, std::string& error);

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// Waits up to `timeout` for a client; invalid socket if none.
    [[nodiscard]] Socket accept(std::chrono::milliseconds timeout) noexcept;
    void close() noexcept { socket_.close(); }

private:
    Socket socket_;
    std::uint16_t port_ = 0;
};

/// The calling thread's last socket error as text.
[[nodiscard]] std::string last_socket_error();

}  // namespace mqlc::net
