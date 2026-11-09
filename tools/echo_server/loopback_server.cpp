#include "loopback_server.h"

#include "core/frame_decoder.h"
#include "core/signal_ids.h"
#include "core/signal_json.h"

#include <chrono>
#include <utility>
#include <vector>

namespace mqlc::tools {

std::optional<std::string> LoopbackServer::echo(std::string_view request) {
    const auto header = core::json::read_signal_header(request);
    if (!header) return std::nullopt;
    // InitLib asks for the server's log path; "" keeps the connector on its default log file.
    const bool log_path = header->id == core::signal::kGetMainLogPath;
    return core::json::ObjectWriter()
        .add_int("Id", header->id)
        .add_int("ChartId", header->chart_id)
        .add_string("Data", log_path ? "" : "echo")
        .finish();
}

LoopbackServer::LoopbackServer(std::uint16_t port, Responder responder)
    : responder_(std::move(responder)),
      listener_(net::Listener::bind_loopback(port, error_)),
      port_(listener_.port()) {
    if (listener_.valid())
        acceptor_ = std::jthread([this](std::stop_token stop) { accept_loop(std::move(stop)); });
}

LoopbackServer::~LoopbackServer() { stop(); }

void LoopbackServer::stop() {
    std::list<std::unique_ptr<Client>> clients;
    {
        const std::scoped_lock lock(mutex_);
        if (stopped_) return;
        stopped_ = true;
    }
    acceptor_.request_stop();
    if (acceptor_.joinable()) acceptor_.join();
    listener_.close();
    {
        const std::scoped_lock lock(mutex_);
        clients.swap(clients_);
    }
    resumed_.notify_all();
    for (auto& client : clients) {
        client->reader.request_stop();
        client->socket->shutdown();
    }
    for (auto& client : clients) {
        if (client->reader.joinable()) client->reader.join();
        client->socket->abort();  // RST: the client sees a crash, and the port has no TIME_WAIT
    }
}

void LoopbackServer::broadcast(std::string_view frame) {
    const std::scoped_lock lock(mutex_);
    for (auto& client : clients_) write_frame(*client, frame);
}

void LoopbackServer::pause_reading(bool paused) {
    {
        const std::scoped_lock lock(mutex_);
        paused_ = paused;
    }
    resumed_.notify_all();
}

std::size_t LoopbackServer::clients() const {
    const std::scoped_lock lock(mutex_);
    std::size_t live = 0;
    for (const auto& client : clients_) live += client->done ? 0u : 1u;
    return live;
}

void LoopbackServer::accept_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        net::Socket accepted = listener_.accept(std::chrono::milliseconds(50));
        if (!accepted.valid()) continue;
        auto client = std::make_unique<Client>();
        client->socket = std::make_shared<net::Socket>(std::move(accepted));
        Client& ref = *client;
        const std::scoped_lock lock(mutex_);
        if (stopped_) return;
        clients_.push_back(std::move(client));
        ref.reader = std::jthread([this, &ref](std::stop_token client_stop) {
            serve(ref, std::move(client_stop));
            ref.done = true;
        });
    }
}

void LoopbackServer::serve(Client& client, std::stop_token stop) {
    core::FrameDecoder decoder;
    std::vector<char> buffer(64 * 1024);
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex_);
            (void)resumed_.wait(lock, stop, [this] { return !paused_; });
        }
        if (stop.stop_requested()) return;
        const std::ptrdiff_t n = client.socket->receive(buffer);
        if (n <= 0) return;
        decoder.feed(std::span<const char>(buffer.data(), static_cast<std::size_t>(n)),
                     [&](std::string_view frame) {
                         ++requests_;
                         if (auto reply = responder_(frame)) write_frame(client, *reply);
                     });
    }
}

void LoopbackServer::write_frame(Client& client, std::string_view frame) {
    std::string framed;
    framed.reserve(frame.size() + 1);
    framed.append(frame).push_back(core::kFrameDelimiter);
    const std::scoped_lock lock(client.write_mutex);
    (void)client.socket->send_all(framed);
}

}  // namespace mqlc::tools
