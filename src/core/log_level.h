// Log severities (InitLib key `LogLevel`). A line is written when its level is at or below the
// configured one: `off` writes nothing, `error` only failures, `info` everything.
#pragma once

#include <cstdint>

namespace mqlc::core {

enum class LogLevel : std::uint8_t { off = 0, error = 1, info = 2 };

}  // namespace mqlc::core
