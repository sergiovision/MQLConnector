// mqlc_bench: request -> reply round-trip latency of the connector session (what DoSendMessage
// does, minus the UTF-16 conversion at the ABI).
//
//   mqlc_bench [--n 10000] [--warmup 200] [--payload 64] [--port P]
//              [--framing delimiter|length] [--delimiter 26] [--corr 0|1]
//
// --corr 1 turns correlation on (CorrelationKey "ReqId"; every request carries one).
//
// Without --port an in-process loopback echo server is started; with --port it talks to an
// external one (tools/echo_server, or FinCore itself on 2022 - every request then reaches the
// server's signal handlers, so use SIGNAL_CHECK_HEALTH-like traffic only).
#include "connector/session.h"
#include "core/signal_json.h"
#include "loopback_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    long n = 10'000;
    long warmup = 200;
    long payload = 64;
    unsigned long port = 0;
    std::string_view framing = "delimiter";
    long delimiter = 26;
    long corr = 0;
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key(argv[i]);
        const char* value = argv[i + 1];
        if (key == "--n") o.n = std::strtol(value, nullptr, 10);
        else if (key == "--warmup") o.warmup = std::strtol(value, nullptr, 10);
        else if (key == "--payload") o.payload = std::strtol(value, nullptr, 10);
        else if (key == "--port") o.port = std::strtoul(value, nullptr, 10);
        else if (key == "--framing") o.framing = value;
        else if (key == "--delimiter") o.delimiter = std::strtol(value, nullptr, 10);
        else if (key == "--corr") o.corr = std::strtol(value, nullptr, 10);
        else return false;
    }
    return argc % 2 == 1 && o.n > 0 && o.warmup >= 0 && o.payload >= 0 && o.port <= 65535 &&
           (o.framing == "delimiter" || o.framing == "length") && o.delimiter >= 1 && o.delimiter <= 31 &&
           (o.corr == 0 || o.corr == 1);
}

double percentile(const std::vector<double>& sorted, double p) {
    const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: mqlc_bench [--n N] [--warmup N] [--payload BYTES] [--port P]\n"
                     "                  [--framing delimiter|length] [--delimiter 1..31] [--corr 0|1]\n");
        return 2;
    }

    std::unique_ptr<mqlc::tools::LoopbackServer> server;
    std::uint16_t port = static_cast<std::uint16_t>(o.port);
    mqlc::core::FramingOptions framing;
    framing.kind = o.framing == "length" ? mqlc::core::FramingKind::length : mqlc::core::FramingKind::delimiter;
    framing.delimiter = static_cast<char>(o.delimiter);
    if (port == 0) {
        server = std::make_unique<mqlc::tools::LoopbackServer>(std::uint16_t{0}, mqlc::tools::LoopbackServer::echo, framing);
        if (!server->listening()) {
            std::fprintf(stderr, "cannot start loopback server: %s\n", server->error().c_str());
            return 1;
        }
        port = server->port();
    }

    constexpr std::int64_t kChart = 4242;
    mqlc::connector::Session session;
    const std::string init = mqlc::core::json::ObjectWriter()
                                 .add_string("Host", "127.0.0.1")
                                 .add_int("Port", port)
                                 .add_int("ChartId", kChart)
                                 .add_int("WaitTimeout", 3000)
                                 .add_string("Framing", o.framing)
                                 .add_int("Delimiter", o.delimiter)
                                 .add_string("CorrelationKey", o.corr != 0 ? "ReqId" : "")
                                 .finish();
    if (session.init(init) != 1) {
        std::fprintf(stderr, "cannot connect to 127.0.0.1:%u\n", unsigned{port});
        return 1;
    }

    mqlc::core::json::ObjectWriter writer;
    writer.add_int("Id", 1001).add_int("ChartId", kChart);
    if (o.corr != 0) writer.add_int("ReqId", 1);
    const std::string request = writer.add_string("Sym", "BENCH")
                                    .add_string("Data", std::string(static_cast<std::size_t>(o.payload), 'x'))
                                    .finish();
    for (long i = 0; i < o.warmup; ++i) (void)session.send_and_wait(request);

    std::vector<double> micros;
    micros.reserve(static_cast<std::size_t>(o.n));
    long failures = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (long i = 0; i < o.n; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string reply = session.send_and_wait(request);
        const auto t1 = std::chrono::steady_clock::now();
        if (reply.empty()) ++failures;
        micros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    session.destroy(kChart);

    std::sort(micros.begin(), micros.end());
    std::printf("round trips  %ld (failures %ld), payload %ld B, %s server, %s framing, correlation %s\n", o.n,
                failures, o.payload, server ? "in-process" : "external", std::string(o.framing).c_str(),
                o.corr != 0 ? "on" : "off");
    std::printf("p50   %9.1f us\np99   %9.1f us\np99.9 %9.1f us\nmax   %9.1f us\n",
                percentile(micros, 0.50), percentile(micros, 0.99), percentile(micros, 0.999),
                micros.back());
    std::printf("total %9.1f ms (%.0f round trips/s)\n", total_ms,
                static_cast<double>(o.n) * 1000.0 / total_ms);
    return failures == 0 ? 0 : 1;
}
