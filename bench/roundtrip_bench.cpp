// mqlc_bench: request -> reply round-trip latency of the connector session (what DoSendMessage
// does, minus the UTF-16 conversion at the ABI).
//
//   mqlc_bench [--n 10000] [--warmup 200] [--payload 64] [--port P]
//              [--framing delimiter|length] [--delimiter 26] [--corr 0|1]
//              [--payload-file FILE] [--protocol json|fbe] [--chart ID] [--first-file FILE]
//
// --chart sets the route the session subscribes (default 4242); with --payload-file against an
// external server it must be the file's ChartId, or the replies land on an unsubscribed route.
// --first-file sends that file once before the warm-up and requires a reply (e.g. a protocol
// handshake such as FinCore's FBE Hello). With --protocol fbe every request gets
// a fresh ReqId written at the envelope's fixed offset (bytes 16..23), so each reply correlates.
//
// --protocol fbe uses the binary path (Session::send_binary, what DoSendBinary does minus the copy
// into the MQL array): length framing, FbeCodec, correlation by the envelope's ReqId. The payload
// file must then hold one whole FBE final-model message (e.g. the *.bin fixtures); without a file a
// synthetic message with a --payload-byte string is sent.
//
// --corr 1 turns correlation on (CorrelationKey "ReqId"; every request carries one).
// --payload-file sends that file's bytes as the request (a whole JSON frame, e.g. the fixtures in
// FinCore/UnitTests/Fixtures/fbe), and the in-process server mirrors every request back verbatim,
// so request and reply have the fixture's size (tools/echo_server --mirror 1 does the same).
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
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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
    const char* payload_file = nullptr;
    std::string_view protocol = "json";
    long long chart = 4242;
    const char* first_file = nullptr;
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
        else if (key == "--payload-file") o.payload_file = value;
        else if (key == "--protocol") o.protocol = value;
        else if (key == "--chart") o.chart = std::strtoll(value, nullptr, 10);
        else if (key == "--first-file") o.first_file = value;
        else return false;
    }
    return argc % 2 == 1 && o.n > 0 && o.warmup >= 0 && o.payload >= 0 && o.port <= 65535 &&
           (o.framing == "delimiter" || o.framing == "length") && o.delimiter >= 1 && o.delimiter <= 31 &&
           (o.corr == 0 || o.corr == 1) && (o.protocol == "json" || o.protocol == "fbe");
}

bool read_file(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::optional<std::string> mirror(std::string_view request) { return std::string(request); }

void put_le(std::string& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

/// A minimal FBE final-model message: size | type 1 | Envelope{ChartId, ReqId, Id, Flags, ObjectId,
/// Value, Sym, Key} | string payload (MQLConnector's tests/fbe/test.fbe TestMessage).
std::string synthetic_fbe(std::int64_t chart, std::int64_t req, std::size_t payload) {
    std::string out(4, '\0');
    put_le(out, 1, 4);
    put_le(out, static_cast<std::uint64_t>(chart), 8);
    put_le(out, static_cast<std::uint64_t>(req), 8);
    put_le(out, 1001, 4);
    put_le(out, 0, 8 + 8 + 4);
    put_le(out, 5, 4);
    out.append("BENCH");
    put_le(out, 0, 4);
    put_le(out, payload, 4);
    out.append(payload, 'x');
    const auto size = static_cast<std::uint32_t>(out.size());
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((size >> (8 * i)) & 0xFFu);
    return out;
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
                     "                  [--framing delimiter|length] [--delimiter 1..31] [--corr 0|1]\n"
                     "                  [--payload-file FILE] [--protocol json|fbe] [--chart ID] [--first-file FILE]\n");
        return 2;
    }

    std::string file_payload;
    if (o.payload_file != nullptr && !read_file(o.payload_file, file_payload)) {
        std::fprintf(stderr, "cannot read %s\n", o.payload_file);
        return 2;
    }
    std::string first_payload;
    if (o.first_file != nullptr && !read_file(o.first_file, first_payload)) {
        std::fprintf(stderr, "cannot read %s\n", o.first_file);
        return 2;
    }
    const bool from_file = o.payload_file != nullptr;
    const bool fbe = o.protocol == "fbe";
    if (fbe) o.framing = "length";  // as InitLib forces it

    std::unique_ptr<mqlc::tools::LoopbackServer> server;
    std::uint16_t port = static_cast<std::uint16_t>(o.port);
    mqlc::core::FramingOptions framing;
    framing.kind = o.framing == "length" ? mqlc::core::FramingKind::length : mqlc::core::FramingKind::delimiter;
    framing.delimiter = static_cast<char>(o.delimiter);
    if (port == 0) {
        server = std::make_unique<mqlc::tools::LoopbackServer>(
            std::uint16_t{0}, from_file || fbe ? mirror : mqlc::tools::LoopbackServer::echo, framing);
        if (!server->listening()) {
            std::fprintf(stderr, "cannot start loopback server: %s\n", server->error().c_str());
            return 1;
        }
        port = server->port();
    }

    const std::int64_t kChart = o.chart;
    mqlc::connector::Session session;
    const std::string init = mqlc::core::json::ObjectWriter()
                                 .add_string("Host", "127.0.0.1")
                                 .add_int("Port", port)
                                 .add_int("ChartId", kChart)
                                 .add_int("WaitTimeout", 3000)
                                 .add_string("Framing", o.framing)
                                 .add_int("Delimiter", o.delimiter)
                                 .add_string("CorrelationKey", o.corr != 0 ? "ReqId" : "")
                                 .add_string("Protocol", fbe ? "fbe" : "tcp")
                                 .finish();
    if (session.init(init) != 1) {
        std::fprintf(stderr, "cannot connect to 127.0.0.1:%u\n", unsigned{port});
        return 1;
    }

    mqlc::core::json::ObjectWriter writer;
    writer.add_int("Id", 1001).add_int("ChartId", kChart);
    if (o.corr != 0) writer.add_int("ReqId", 1);
    std::string request =
        from_file ? file_payload
        : fbe     ? synthetic_fbe(kChart, 1, static_cast<std::size_t>(o.payload))
                  : writer.add_string("Sym", "BENCH")
                        .add_string("Data", std::string(static_cast<std::size_t>(o.payload), 'x'))
                        .finish();
    constexpr std::size_t kReqIdOffset = 16;  // u32 size | u32 type | ChartId i64 | ReqId i64
    std::uint64_t next_req = 1;
    const auto round_trip = [&] {
        if (fbe) {
            if (request.size() >= kReqIdOffset + 8) {  // a fresh ReqId: a late reply never matches
                const std::uint64_t req = next_req++;
                for (int i = 0; i < 8; ++i)
                    request[kReqIdOffset + static_cast<std::size_t>(i)] =
                        static_cast<char>((req >> (8 * i)) & 0xFFu);
            }
            return session.send_binary(request).value_or(std::string());
        }
        return session.send_and_wait(request);
    };
    if (!first_payload.empty()) {
        const std::string answer = fbe ? session.send_binary(first_payload).value_or(std::string())
                                       : session.send_and_wait(first_payload);
        if (answer.empty()) {
            std::fprintf(stderr, "no reply to --first-file %s\n", o.first_file);
            return 1;
        }
    }
    for (long i = 0; i < o.warmup; ++i) (void)round_trip();

    std::vector<double> micros;
    micros.reserve(static_cast<std::size_t>(o.n));
    long failures = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (long i = 0; i < o.n; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string reply = round_trip();
        const auto t1 = std::chrono::steady_clock::now();
        if (reply.empty()) ++failures;
        micros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    session.destroy(kChart);

    std::sort(micros.begin(), micros.end());
    std::printf("round trips  %ld (failures %ld), %s, request %zu B%s, %s server, %s framing, correlation %s\n",
                o.n, failures, fbe ? "fbe" : "json", request.size(), from_file ? (server ? " (file, mirrored)" : " (file)") : "",
                server ? "in-process" : "external", std::string(o.framing).c_str(),
                fbe ? "ReqId" : (o.corr != 0 ? "on" : "off"));
    std::printf("p50   %9.1f us\np99   %9.1f us\np99.9 %9.1f us\nmax   %9.1f us\n",
                percentile(micros, 0.50), percentile(micros, 0.99), percentile(micros, 0.999),
                micros.back());
    std::printf("total %9.1f ms (%.0f round trips/s)\n", total_ms,
                static_cast<double>(o.n) * 1000.0 / total_ms);
    return failures == 0 ? 0 : 1;
}
