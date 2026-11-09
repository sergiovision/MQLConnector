// The signal ids (`Id` of a frame) the connector itself sends. Every other id is opaque to it:
// frames are routed by `ChartId` only.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace mqlc::core::signal {

inline constexpr std::int64_t kLevelsForSymbol = 1023;  // SIGNAL_LEVELS4SYMBOL
inline constexpr std::int64_t kGetMainLogPath = 1026;   // SIGNAL_GETMAINLOGPATH
inline constexpr std::int64_t kGetFromCache = 1033;     // SIGNAL_GET_SIGNAL_FROMCACHE

/// "SIGNAL_LEVELS4SYMBOL" for 1023, the number itself for ids the connector does not send.
[[nodiscard]] inline std::string name(std::int64_t id) {
    static constexpr std::array<std::pair<std::int64_t, std::string_view>, 3> kNames{{
        {kLevelsForSymbol, "SIGNAL_LEVELS4SYMBOL"},
        {kGetMainLogPath, "SIGNAL_GETMAINLOGPATH"},
        {kGetFromCache, "SIGNAL_GET_SIGNAL_FROMCACHE"},
    }};
    for (const auto& [value, text] : kNames) {
        if (value == id) return std::string(text);
    }
    return std::to_string(id);
}

}  // namespace mqlc::core::signal
