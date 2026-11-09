// A loopback stand-in for a server's messaging endpoint (FinCore's by default): framed JSON over
// TCP, replies on the asking connection, broadcasts to all. Any framing the connector supports
// (0x1A by default). Used by the integration tests, the benchmark and the echo_server tool.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/framer.h"
#include "net/socket.h"

namespace mqlc::tools {

class LoopbackServer {
public:
    /// Maps a request frame to the reply frame for the same connection (nullopt: no reply).
    using Responder = std::function<std::optional<std::string>(std::string_view request)>;

    /// The default responder: `{"Id":<Id>,"ChartId":<ChartId>,"Data":"echo"}` (`Data` is "" for the
    /// log-path request, so a connector keeps its own log file), plus `"ReqId":<ReqId>` verbatim
    /// when the request has one, for correlated clients.
    [[nodiscard]] static std::optional<std::string> echo(std::string_view request);

    /// Listens on 127.0.0.1:`port` (0 = any free port); check `listening()`.
    explicit LoopbackServer(std::uint16_t port = 0, Responder responder = echo,
                            core::FramingOptions framing = {});
    ~LoopbackServer();

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] bool listening() const noexcept { return listener_.valid(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Sends `frame` to every connected client, as the server does for broadcast signals.
    void broadcast(std::string_view frame);
    /// Sends `bytes` unframed to every connected client (for malformed-stream tests).
    void broadcast_raw(std::string_view bytes);
    /// While paused, connections are not read, so the clients' sends eventually block.
    void pause_reading(bool paused);

    [[nodiscard]] std::size_t clients() const;
    [[nodiscard]] std::uint64_t requests() const noexcept { return requests_.load(); }
    /// Connections accepted so far (a reconnect counts again).
    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_.load(); }

    /// Drops every connection abruptly (RST) and stops listening. Idempotent.
    void stop();

private:
    struct Client {
        std::shared_ptr<net::Socket> socket;
        std::mutex write_mutex;
        std::atomic<bool> done{false};  // the connection closed
        std::jthread reader;
    };

    void accept_loop(std::stop_token stop);
    void serve(Client& client, std::stop_token stop);
    void write_frame(Client& client, std::string_view frame) const;
    static void write_raw(Client& client, std::string_view bytes);

    const Responder responder_;
    const core::FramingOptions framing_;
    const std::unique_ptr<core::Framer> encoder_;  // encode() only: const, any thread
    std::string error_;
    net::NetworkScope network_;
    net::Listener listener_;
    std::uint16_t port_ = 0;
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> accepted_{0};

    mutable std::mutex mutex_;
    std::condition_variable_any resumed_;
    std::list<std::unique_ptr<Client>> clients_;  // guarded by mutex_
    bool paused_ = false;                         // guarded by mutex_
    bool stopped_ = false;                        // guarded by mutex_

    std::jthread acceptor_;  // last: started after everything it uses
};

}  // namespace mqlc::tools
