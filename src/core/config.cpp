#include "core/config.h"

#include "core/signal_json.h"

#include <array>
#include <filesystem>
#include <optional>
#include <utility>

namespace mqlc::core {
namespace {

enum Field : std::size_t {
    kHost,
    kPort,
    kAccount,
    kProtocol,
    kChartId,
    kWaitOp,
    kWaitTimeout,
    kProfile,
    kRouteKey,
    kCorrelationKey,
    kFraming,
    kDelimiter,
    kLogPath,
    kLogLevel,
    kCount
};
constexpr std::array<std::string_view, kCount> kKeys{
    "Host",    "Port",     "Account",        "Protocol", "ChartId",   "WaitOp",  "WaitTimeout",
    "Profile", "RouteKey", "CorrelationKey", "Framing",  "Delimiter", "LogPath", "LogLevel"};
static_assert(kCount <= 32, "ParsedConfig::given is a 32-bit mask");

using Warnings = std::vector<std::string>;

void read_text(const json::Value& value, std::string_view key, std::string& target, Warnings& warnings) {
    if (value.kind == json::Kind::missing) return;
    if (auto text = json::to_string(value)) {
        target = std::move(*text);
    } else {
        warnings.push_back(std::string(key) + " is not a string or number, using default");
    }
}

/// A member name: a non-empty string (or empty when `allow_empty`).
void read_key_name(const json::Value& value, std::string_view key, bool allow_empty, std::string& target,
                   Warnings& warnings) {
    if (value.kind == json::Kind::missing) return;
    auto text = value.kind == json::Kind::string ? json::to_string(value) : std::nullopt;
    if (!text || (text->empty() && !allow_empty)) {
        warnings.push_back(std::string(key) + " must be " + (allow_empty ? "a string" : "a non-empty string") +
                           ", using default");
        return;
    }
    target = std::move(*text);
}

std::optional<std::int64_t> read_int(const json::Value& value, std::string_view key, Warnings& warnings) {
    if (value.kind == json::Kind::missing) return std::nullopt;
    auto number = json::to_int64(value);
    if (!number) warnings.push_back(std::string(key) + " is not an integer, using default");
    return number;
}

void read_millis(const json::Value& value, std::string_view key, std::chrono::milliseconds fallback,
                 std::chrono::milliseconds& target, Warnings& warnings) {
    if (const auto ms = read_int(value, key, warnings)) {
        target = *ms > 0 ? std::chrono::milliseconds(*ms) : fallback;
    }
}

bool equals_ascii_nocase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

/// One of `choices` (case-insensitive), by its to_string() spelling.
template <class Enum, std::size_t N>
void read_choice(const json::Value& value, std::string_view key, const std::array<Enum, N>& choices,
                 Enum& target, Warnings& warnings) {
    if (value.kind == json::Kind::missing) return;
    const auto text = value.kind == json::Kind::string ? json::to_string(value) : std::nullopt;
    if (text) {
        for (const Enum choice : choices) {
            if (equals_ascii_nocase(*text, to_string(choice))) {
                target = choice;
                return;
            }
        }
    }
    std::string allowed;
    for (const Enum choice : choices) {
        if (!allowed.empty()) allowed += '|';
        allowed += to_string(choice);
    }
    warnings.push_back(std::string(key) + " must be " + allowed + ", using default");
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return {std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())};
}

}  // namespace

std::string_view to_string(ProfileKind kind) noexcept {
    return kind == ProfileKind::generic ? "generic" : "fincore";
}

std::string_view to_string(FramingKind kind) noexcept {
    return kind == FramingKind::length ? "length" : "delimiter";
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::off: return "off";
        case LogLevel::error: return "error";
        case LogLevel::info: break;
    }
    return "info";
}

FramingOptions Config::framing_options() const noexcept {
    FramingOptions options;
    options.kind = framing;
    options.delimiter = delimiter;
    return options;
}

std::string Config::describe() const {
    return json::ObjectWriter()
        .add_string("host", host)
        .add_int("port", port)
        .add_string("account", account)
        .add_string("protocol", protocol)
        .add_int("chartId", chart_id)
        .add_int("waittimeout", wait_timeout.count())
        .add_int("waitop", wait_op.count())
        .add_string("profile", to_string(profile))
        .add_string("routeKey", route_key)
        .add_string("correlationKey", correlation_key)
        .add_string("framing", to_string(framing))
        .add_int("delimiter", static_cast<unsigned char>(delimiter))
        .add_string("logPath", log_path)
        .add_string("logLevel", to_string(log_level))
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
    for (std::size_t i = 0; i < kCount; ++i) {
        if (v[i].kind != json::Kind::missing) parsed.given |= 1u << i;
    }

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

    read_choice(v[kProfile], kKeys[kProfile], std::array{ProfileKind::fincore, ProfileKind::generic},
                c.profile, warnings);
    read_key_name(v[kRouteKey], kKeys[kRouteKey], false, c.route_key, warnings);
    read_key_name(v[kCorrelationKey], kKeys[kCorrelationKey], true, c.correlation_key, warnings);
    read_choice(v[kFraming], kKeys[kFraming], std::array{FramingKind::delimiter, FramingKind::length},
                c.framing, warnings);
    if (v[kDelimiter].kind != json::Kind::missing) {
        const auto delimiter = json::to_int64(v[kDelimiter]);
        if (delimiter && *delimiter >= 1 && *delimiter <= 31) {
            c.delimiter = static_cast<char>(*delimiter);
        } else {
            warnings.emplace_back("Delimiter must be an integer 1..31, using default 26");
        }
    }
    if (v[kLogPath].kind != json::Kind::missing) {
        std::string path;
        read_key_name(v[kLogPath], kKeys[kLogPath], true, path, warnings);
        if (!path.empty() && !path_from_utf8(path).is_absolute()) {
            warnings.push_back("LogPath \"" + path + "\" is not absolute, using default");
        } else {
            c.log_path = std::move(path);
        }
    }
    read_choice(v[kLogLevel], kKeys[kLogLevel], std::array{LogLevel::off, LogLevel::error, LogLevel::info},
                c.log_level, warnings);
    return parsed;
}

std::vector<std::string> ignored_keys(const Config& active, const ParsedConfig& later) {
    const Config& c = later.config;
    const std::array<bool, kCount> differs{
        c.host != active.host,
        c.port != active.port,
        c.account != active.account,
        c.protocol != active.protocol,
        false,  // ChartId: every program has its own
        c.wait_op != active.wait_op,
        c.wait_timeout != active.wait_timeout,
        c.profile != active.profile,
        c.route_key != active.route_key,
        c.correlation_key != active.correlation_key,
        c.framing != active.framing,
        c.delimiter != active.delimiter,
        c.log_path != active.log_path,
        c.log_level != active.log_level,
    };
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < kCount; ++i) {
        if ((later.given & (1u << i)) != 0 && differs[i]) keys.emplace_back(kKeys[i]);
    }
    return keys;
}

}  // namespace mqlc::core
