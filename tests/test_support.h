// Shared helpers for the integration tests.
#pragma once

#include "core/signal_json.h"
#include "loopback_server.h"
#include "profiles/fincore/signal_ids.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace mqlc::test {

using namespace std::chrono_literals;

/// Polls `condition` until it holds or `timeout` passes.
inline bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

inline std::string member(std::string_view frame, std::string_view key) {
    const std::array<std::string_view, 1> keys{key};
    std::array<core::json::Value, 1> values{};
    if (!core::json::read_members(frame, keys, values)) return "<invalid>";
    return core::json::payload_text(values[0]);
}

/// Echoes Id/ChartId and the request's own Data, answers SIGNAL_GETMAINLOGPATH with a temp file
/// (so InitLib never logs into the working directory), and stays silent for `silent_id`.
inline tools::LoopbackServer::Responder fincore_like(std::int64_t silent_id = -1) {
    return [silent_id](std::string_view request) -> std::optional<std::string> {
        const auto header = core::json::read_signal_header(request);
        if (!header || header->id == silent_id) return std::nullopt;
        std::string data = member(request, "Data");
        if (header->id == profiles::fincore::signal::kGetMainLogPath)
            data = (std::filesystem::temp_directory_path() / "mqlc_tests.log").string();
        return core::json::ObjectWriter()
            .add_int("Id", header->id)
            .add_int("ChartId", header->chart_id)
            .add_string("Data", data)
            .finish();
    };
}

inline std::string init_json(std::uint16_t port, std::int64_t chart_id, int wait_timeout_ms = 2000) {
    return core::json::ObjectWriter()
        .add_string("Host", "127.0.0.1")
        .add_int("Port", port)
        .add_string("Account", "test")
        .add_int("ChartId", chart_id)
        .add_int("WaitTimeout", wait_timeout_ms)
        .finish();
}

inline std::string request_json(std::int64_t id, std::int64_t chart_id, std::string_view data = "") {
    return core::json::ObjectWriter()
        .add_int("Id", id)
        .add_int("ChartId", chart_id)
        .add_string("Sym", "EURUSD")
        .add_string("Data", data)
        .finish();
}

/// A port nothing listens on (bound once, then released).
inline std::uint16_t free_port() {
    tools::LoopbackServer probe;
    const std::uint16_t port = probe.port();
    probe.stop();
    return port;
}

}  // namespace mqlc::test
