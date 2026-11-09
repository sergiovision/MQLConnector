// InitLib parameters. MQL sends them as one JSON object whose values are strings
// (CBacktestDecisionClient) or numbers (MQL5Utils::SetupTcpConnection); both are accepted.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mqlc::core {

struct Config {
    static constexpr std::chrono::milliseconds kDefaultWaitTimeout{3000};
    static constexpr std::chrono::milliseconds kDefaultWaitOp{300};

    std::string host = "127.0.0.1";
    std::uint16_t port = 2022;
    std::string account = "1000000";
    std::string protocol = "tcp";
    std::int64_t chart_id = 0;
    std::chrono::milliseconds wait_timeout = kDefaultWaitTimeout;  // reply / reconnect budget
    std::chrono::milliseconds wait_op = kDefaultWaitOp;  // .NET poll interval: parsed, unused
    bool has_parameters = false;  // a non-empty parameter object was given

    /// One-line JSON summary for the log.
    [[nodiscard]] std::string describe() const;
};

struct ParsedConfig {
    Config config;
    std::vector<std::string> warnings;  // values that were ignored, for the log
};

/// Parses InitLib's JSON. Empty input, a non-object or any bad value never fails: the affected
/// settings keep their defaults and a warning explains why. Values <= 0 for WaitOp/WaitTimeout
/// mean "default", as before.
[[nodiscard]] ParsedConfig parse_config(std::string_view json_utf8);

}  // namespace mqlc::core
