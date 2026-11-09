// echo_server: a FinCore stand-in for benchmarks. Answers every 0x1A-framed request with
// {"Id":<Id>,"ChartId":<ChartId>,"Data":"echo"} on the same connection.
//
//   echo_server [--port 2023]
//
// Point mql/Scripts/ConnectorBench.mq5 (or mqlc_bench --port) at it.
#include "loopback_server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_quit{false};

void on_signal(int) { g_quit = true; }

}  // namespace

int main(int argc, char** argv) {
    unsigned long port = 2023;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--port" && i + 1 < argc) {
            port = std::strtoul(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: echo_server [--port N]\n");
            return 2;
        }
    }
    if (port == 0 || port > 65535) {
        std::fprintf(stderr, "echo_server: bad port\n");
        return 2;
    }

    mqlc::tools::LoopbackServer server(static_cast<std::uint16_t>(port));
    if (!server.listening()) {
        std::fprintf(stderr, "echo_server: cannot listen on %lu: %s\n", port, server.error().c_str());
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("echo_server listening on 127.0.0.1:%u (Ctrl+C to stop)\n", unsigned{server.port()});
    std::fflush(stdout);

    std::uint64_t reported = 0;
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (const auto n = server.requests(); n != reported) {
            std::printf("%llu requests, %zu clients\n", static_cast<unsigned long long>(n), server.clients());
            std::fflush(stdout);
            reported = n;
        }
    }
    server.stop();
    return 0;
}
