// echo_server: a server stand-in for benchmarks and manual tests. Answers every request with
// {"Id":<Id>,"ChartId":<ChartId>,"Data":"echo"} (plus "ReqId" when the request has one) on the
// same connection.
//
//   echo_server [--port 2023] [--framing delimiter|length] [--delimiter 26]
//
// Point mql/Scripts/ConnectorBench.mq5 (or mqlc_bench --port) at it, with the same framing.
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

int usage() {
    std::fprintf(stderr, "usage: echo_server [--port N] [--framing delimiter|length] [--delimiter 1..31]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned long port = 2023;
    long delimiter = 26;
    mqlc::core::FramingOptions framing;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (i + 1 >= argc) return usage();
        const std::string_view value(argv[++i]);
        if (arg == "--port") {
            port = std::strtoul(value.data(), nullptr, 10);
        } else if (arg == "--framing" && (value == "delimiter" || value == "length")) {
            framing.kind = value == "length" ? mqlc::core::FramingKind::length : mqlc::core::FramingKind::delimiter;
        } else if (arg == "--delimiter") {
            delimiter = std::strtol(value.data(), nullptr, 10);
        } else {
            return usage();
        }
    }
    if (port == 0 || port > 65535) {
        std::fprintf(stderr, "echo_server: bad port\n");
        return 2;
    }
    if (delimiter < 1 || delimiter > 31) {
        std::fprintf(stderr, "echo_server: the delimiter must be 1..31\n");
        return 2;
    }
    framing.delimiter = static_cast<char>(delimiter);

    mqlc::tools::LoopbackServer server(static_cast<std::uint16_t>(port), mqlc::tools::LoopbackServer::echo,
                                       framing);
    if (!server.listening()) {
        std::fprintf(stderr, "echo_server: cannot listen on %lu: %s\n", port, server.error().c_str());
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (framing.kind == mqlc::core::FramingKind::length) {
        std::printf("echo_server listening on 127.0.0.1:%u, length-prefix framing (Ctrl+C to stop)\n",
                    unsigned{server.port()});
    } else {
        std::printf("echo_server listening on 127.0.0.1:%u, delimiter %ld framing (Ctrl+C to stop)\n",
                    unsigned{server.port()}, delimiter);
    }
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
