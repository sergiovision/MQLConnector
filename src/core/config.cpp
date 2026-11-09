#include "core/config.h"

#include "core/signal_json.h"

#include <array>
#include <optional>
#include <utility>

namespace mqlc::core {
namespace {

enum Field : std::size_t { kHost, kPort, kAccount, kProtocol, kChartId, kWaitOp, kWaitTimeout, kCount };
constexpr std::array<std::string_view, kCount> kKeys{"Host",    "Port",   "Account",    "Protocol",
                                                      "ChartId", "WaitOp", "WaitTimeout"};

void read_text(const json::Value& value, std::string_view key, std::string& target,
               std::vector<std::string>& warnings) {
    if (value.kind == json::Kind::missing) return;
    if (auto text = json::to_string(value)) {
        target = std::move(*text);
    } else {
        warnings.push_back(std::string(key) + " is not a string or number, using default");
    }
}

std::optional<std::int64_t> read_int(const json::Value& value, std::string_view key,
                                     std::vector<std::string>& warnings) {
    if (value.kind == json::Kind::missing) return std::nullopt;
    auto number = json::to_int64(value);
    if (!number) warnings.push_back(std::string(key) + " is not an integer, using default");
    return number;
}

void read_millis(const json::Value& value, std::string_view key, std::chrono::milliseconds fallback,
                 std::chrono::milliseconds& target, std::vector<std::string>& warnings) {
    if (const auto ms = read_int(value, key, warnings)) {
        target = *ms > 0 ? std::chrono::milliseconds(*ms) : fallback;
    }
}

}  // namespace

std::string Config::describe() const {
    return json::ObjectWriter()
        .add_string("host", host)
        .add_int("port", port)
        .add_string("account", account)
        .add_string("protocol", protocol)
        .add_int("chartId", chart_id)
        .add_int("waittimeout", wait_timeout.count())
        .add_int("waitop", wait_op.count())
        .finish();
}

ParsedConfig parse_config(std::string_view json_utf8) {
    ParsedConfig parsed;
    Config& c = parsed.config;
    auto& warnings = parsed.warnings;

    std::array<json::Value, kCount> v{};
    if (json_utf8.find_first_not_of(" \t\r\n") == std::string_view::npos) return parsed;
    if (!json::read_members(json_utf8, kKeys, v)) {
        warnings.emplace_back("InitLib parameters are not a JSON object, using defaults");
        return parsed;
    }
    c.has_parameters = json_utf8.find('"') != std::string_view::npos;  // "{}" counts as empty

    read_text(v[kHost], kKeys[kHost], c.host, warnings);
    read_text(v[kAccount], kKeys[kAccount], c.account, warnings);
    read_text(v[kProtocol], kKeys[kProtocol], c.protocol, warnings);
    if (const auto port = read_int(v[kPort], kKeys[kPort], warnings)) {
        if (*port > 0 && *port <= 65535) {
            c.port = static_cast<std::uint16_t>(*port);
        } else {
            warnings.emplace_back("Port is out of range, using default");
        }
    }
    if (const auto chart = read_int(v[kChartId], kKeys[kChartId], warnings)) c.chart_id = *chart;
    read_millis(v[kWaitOp], kKeys[kWaitOp], Config::kDefaultWaitOp, c.wait_op, warnings);
    read_millis(v[kWaitTimeout], kKeys[kWaitTimeout], Config::kDefaultWaitTimeout, c.wait_timeout,
                warnings);
    return parsed;
}

}  // namespace mqlc::core
