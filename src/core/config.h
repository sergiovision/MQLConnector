// InitLib parameters. MQL sends them as one JSON object whose values are strings
// (CBacktestDecisionClient) or numbers (MQL5Utils::SetupTcpConnection); both are accepted.
//
// Configuration is process-wide: the first InitLib of a connection lifetime sets it, later calls
// only add references (ignored_keys() lists what they asked for in vain).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/framer.h"
#include "core/log_level.h"

namespace mqlc::core {

/// `fincore`: FinCore's protocol extras (log-path handshake, GetLevels, GetSignalCache).
/// `generic`: none of them; any server that routes by the route key works.
enum class ProfileKind : std::uint8_t { fincore, generic };

struct Config {
    static constexpr std::chrono::milliseconds kDefaultWaitTimeout{3000};
    static constexpr std::chrono::milliseconds kDefaultWaitOp{300};
    static constexpr char kDefaultRouteKey[] = "ChartId";

    std::string host = "127.0.0.1";
    std::uint16_t port = 2022;
    std::string account = "1000000";
    std::string protocol = "tcp";  // as given; anything but "tcp" falls back to tcp
    std::int64_t chart_id = 0;     // the route this InitLib subscribes, whatever RouteKey says
    std::chrono::milliseconds wait_timeout = kDefaultWaitTimeout;  // reply / reconnect budget
    std::chrono::milliseconds wait_op = kDefaultWaitOp;  // .NET poll interval: parsed, unused
    ProfileKind profile = ProfileKind::fincore;
    std::string route_key = kDefaultRouteKey;
    std::string correlation_key;  // empty: correlation off
    FramingKind framing = FramingKind::delimiter;
    char delimiter = kFrameDelimiter;  // 1..31
    std::string log_path;              // UTF-8, absolute; empty: %TEMP%\MQLConnector.log
    LogLevel log_level = LogLevel::info;
    bool has_parameters = false;  // a non-empty parameter object was given

    [[nodiscard]] FramingOptions framing_options() const noexcept;

    /// One-line JSON summary for the log.
    [[nodiscard]] std::string describe() const;
};

struct ParsedConfig {
    Config config;
    std::vector<std::string> warnings;  // values that were ignored, for the log
    std::uint32_t given = 0;            // bit per key present in the input (see config.cpp)
};

/// Parses InitLib's JSON. Empty input, a non-object or any bad value never fails: the affected
/// settings keep their defaults and a warning explains why. Values <= 0 for WaitOp/WaitTimeout
/// mean "default", as before.
[[nodiscard]] ParsedConfig parse_config(std::string_view json_utf8);

/// The process-wide keys `later` sets to something other than `active` holds (ChartId is
/// per-program and never listed).
[[nodiscard]] std::vector<std::string> ignored_keys(const Config& active, const ParsedConfig& later);

[[nodiscard]] std::string_view to_string(ProfileKind kind) noexcept;
[[nodiscard]] std::string_view to_string(FramingKind kind) noexcept;
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

}  // namespace mqlc::core
