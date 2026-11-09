// The connector's behaviour, independent of the C ABI: one ref-counted connection to the server
// shared by every program in the terminal, per-route mailboxes for replies, and the log.
//
// Threads: MQL calls arrive on any chart's thread; the transport adds a reader thread (which
// calls on_frame) and a writer thread; the logger has its own writer. All methods are
// thread-safe. Strings are UTF-8 here; exports.cpp converts at the MQL boundary.
//
// Session is not a singleton: process_session() is the one the DLL exports use, tests build
// their own, and a handle-based multi-connection API could hold several.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/codec.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/mailbox.h"
#include "net/transport.h"
#include "profiles/profile.h"

namespace mqlc::connector {

class Session {
public:
    using TransportFactory = std::function<std::unique_ptr<net::Transport>(
        const core::Config&, net::Transport::FrameHandler, net::LogFn)>;

    static constexpr std::string_view kNotInitialized = "Failed to initialize MQLConnector.dll";

    /// Connects over TCP (net::make_tcp_transport).
    Session();
    explicit Session(TransportFactory factory, core::Logger::Options log_options = {});
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// InitLib: 1 if connected, else 0. The first call configures and connects; every call
    /// counts a reference and subscribes its ChartId.
    int init(std::string_view json);
    /// DestroyLib: drops the chart's mailbox and a reference; the last one closes the connection.
    void destroy(std::int64_t chart_id);

    /// DoSendMessage: sends and waits up to WaitTimeout for the reply on the message's route: the
    /// first frame queued there or, with correlation, the first one carrying the request's value.
    /// "" on timeout, bad input, or no connection.
    std::string send_and_wait(std::string_view json);
    /// DoPostMessage: queues the message for sending and returns at once.
    void post(std::string_view json);
    /// DoListenMessage: the next frame queued for `route`, or "".
    std::string listen(std::int64_t route);
    /// GetLevels / GetSignalCache: the profile's business (fincore: FinCore requests; generic: "").
    std::string get_levels(std::string_view sym, std::int64_t chart_id);
    std::string get_signal_cache(std::string_view key, std::string_view sym, std::int64_t chart_id);

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] int references() const;
    [[nodiscard]] core::Logger& logger() noexcept { return logger_; }

private:
    // One connection lifetime (InitLib .. last DestroyLib), immutable once published. Callers
    // hold a shared_ptr copy for the duration of a request, so a concurrent DestroyLib never
    // frees it under them. Members are destroyed bottom-up: the transport (and with it the
    // reader thread, which uses the codec) goes first.
    struct Link {
        core::Config config;
        std::unique_ptr<profiles::Profile> profile;
        std::unique_ptr<core::Codec> codec;
        std::optional<char> delimiter;  // the framing's reserved byte, for Codec::protect
        std::stop_source stop;          // requested when the link retires: ends every wait on it
        std::unique_ptr<net::Transport> transport;
    };
    class LinkChannel;

    [[nodiscard]] std::shared_ptr<Link> link() const;
    void start(const core::ParsedConfig& parsed);
    std::optional<std::string> request(Link& link, std::string_view frame, const core::Envelope& envelope);
    void on_frame(const core::Codec& codec, std::string_view frame);
    void log(std::string_view message) noexcept { logger_.log(core::LogLevel::info, message); }
    void log_error(std::string_view message) noexcept { logger_.log(core::LogLevel::error, message); }

    const TransportFactory factory_;
    core::Logger logger_;
    core::MailboxRegistry mailboxes_;

    mutable std::mutex lifecycle_mutex_;  // serialises init/destroy; held across the first connect
    int references_ = 0;                  // guarded by lifecycle_mutex_

    mutable std::mutex link_mutex_;  // guards only the pointer below, never held while waiting
    std::shared_ptr<Link> link_;

    // Correlation is on but a request lacked the member: warned once per session (any thread).
    std::atomic<bool> uncorrelated_warned_{false};
};

/// The process-wide session behind the DLL exports, created on first use and never destroyed
/// (see core::NoDestructor).
[[nodiscard]] Session& process_session();

}  // namespace mqlc::connector
