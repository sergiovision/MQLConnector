#include "net/socket.h"
#include "net/transport.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace mqlc::net {
namespace {

using core::LogLevel;

class TcpTransport final : public Transport {
public:
    TcpTransport(TcpOptions options, FrameHandler on_frame, LogFn log)
        : options_(std::move(options)),
          on_frame_(std::move(on_frame)),
          log_(std::move(log)),
          framer_(core::make_framer(options_.framing)),
          sink_(*this),
          io_thread_([this](std::stop_token stop) { guarded("reader", [&] { io_loop(stop); }); }),
          writer_thread_([this](std::stop_token stop) { guarded("writer", [&] { write_loop(stop); }); }) {}

    ~TcpTransport() override { stop(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        if (connected_) return true;
        kick_ = true;
        changed_.notify_all();
        (void)changed_.wait_for(lock, timeout, [this] { return connected_ || stopping_; });
        return connected_;
    }

    bool connected() const noexcept override { return connected_.load(std::memory_order_acquire); }

    bool send(std::string_view frame) override {
        std::string framed;
        framed.reserve(frame.size() + framer_->overhead());
        if (!framer_->encode(frame, framed)) return false;  // encode is const: safe on any thread
        {
            const std::scoped_lock lock(mutex_);
            if (!socket_ || queued_bytes_ + framed.size() > options_.max_queued_bytes) return false;
            queued_bytes_ += framed.size();
            outbox_.push_back(std::move(framed));
        }
        changed_.notify_all();
        return true;
    }

    void stop() override {
        {
            const std::scoped_lock lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            if (socket_) socket_->shutdown();  // breaks the reader's recv and a writer's send
        }
        io_thread_.request_stop();
        writer_thread_.request_stop();
        changed_.notify_all();
        if (io_thread_.joinable()) io_thread_.join();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

private:
    template <class Body>
    void guarded(const char* thread_name, Body&& body) noexcept {
        try {
            body();
        } catch (const std::exception& e) {
            log(LogLevel::error, std::string("TCP ") + thread_name + " thread stopped: " + e.what());
        } catch (...) {
            log(LogLevel::error, std::string("TCP ") + thread_name + " thread stopped: unknown exception");
        }
    }

    void log(LogLevel level, std::string_view message) const noexcept {
        try {
            if (log_) log_(level, message);
        } catch (...) {
        }
    }

    [[nodiscard]] std::string endpoint() const {
        return options_.host + ":" + std::to_string(options_.port);
    }

    // Connects, reads until the connection drops, backs off, repeats until stopped.
    void io_loop(std::stop_token stop) {
        std::vector<char> buffer(64 * 1024);
        auto backoff = options_.initial_backoff;
        bool failure_logged = false;
        while (!stop.stop_requested()) {
            std::string error;
            Socket fresh = Socket::connect(options_.host, options_.port,
                                           options_.connect_attempt_timeout, stop, error);
            if (fresh.valid()) {
                backoff = options_.initial_backoff;
                failure_logged = false;
                const auto socket = std::make_shared<Socket>(std::move(fresh));
                if (!publish(socket)) break;  // stopped while connecting
                log(LogLevel::info, "Connected to " + endpoint());
                read_until_closed(*socket, buffer, stop);
                retire(*socket);
                if (stop.stop_requested()) break;
                log(LogLevel::info, "Disconnected from " + endpoint() + ", reconnecting");
            } else if (!failure_logged && !stop.stop_requested()) {
                log(LogLevel::error, "Connect to " + endpoint() + " failed: " + error + "; retrying");
                failure_logged = true;
            }

            std::unique_lock lock(mutex_);
            (void)changed_.wait_for(lock, stop, backoff, [this] { return kick_; });
            kick_ = false;
            backoff = std::min(backoff * 2, options_.max_backoff);
        }
    }

    bool publish(const std::shared_ptr<Socket>& socket) {
        {
            const std::scoped_lock lock(mutex_);
            if (stopping_) return false;
            socket_ = socket;
            connected_ = true;
        }
        changed_.notify_all();
        return true;
    }

    void retire(Socket& socket) {
        std::size_t dropped = 0;
        {
            const std::scoped_lock lock(mutex_);
            socket_.reset();
            connected_ = false;
            dropped = outbox_.size();  // requests for a connection that is gone
            outbox_.clear();
            queued_bytes_ = 0;
        }
        changed_.notify_all();
        socket.shutdown();  // wakes a writer still inside send() on this socket
        framer_->reset();   // a frame cut by the disconnect can never complete
        if (dropped > 0) log(LogLevel::error, std::to_string(dropped) + " queued frames dropped on disconnect");
    }

    void read_until_closed(Socket& socket, std::vector<char>& buffer, const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            const std::ptrdiff_t n = socket.receive(buffer);
            if (n <= 0) return;
            const std::span<const char> bytes(buffer.data(), static_cast<std::size_t>(n));
            const core::FeedResult result = framer_->feed(bytes, sink_);
            if (result.dropped > 0) {
                log(LogLevel::error, "OnReceived: frame exceeds " +
                                         std::to_string(options_.framing.max_frame_bytes) +
                                         " bytes without a delimiter, dropped");
            }
            if (result.corrupt) {
                // A length-prefixed stream has no marker to resynchronise on: start over on a new
                // connection (retire() resets the framer, io_loop reconnects).
                log(LogLevel::error, "OnReceived: invalid length prefix (below 4 or over " +
                                         std::to_string(options_.framing.max_frame_bytes) +
                                         " bytes), closing the connection to resynchronise");
                return;
            }
        }
    }

    // Reader thread only: hands each frame to the session, containing its exceptions.
    void deliver(std::string_view frame) noexcept {
        try {
            on_frame_(frame);
        } catch (const std::exception& e) {
            log(LogLevel::error, std::string("Frame handler failed: ") + e.what());
        } catch (...) {
            log(LogLevel::error, "Frame handler failed: unknown exception");
        }
    }

    struct ReaderSink final : core::FrameSink {
        explicit ReaderSink(TcpTransport& owner) noexcept : owner_(owner) {}
        void on_frame(std::string_view frame) override { owner_.deliver(frame); }
        TcpTransport& owner_;
    };

    // Sends queued frames in batches: everything queued while one send was in flight goes out
    // in a single write.
    void write_loop(std::stop_token stop) {
        std::deque<std::string> batch;
        std::string buffer;
        while (true) {
            std::shared_ptr<Socket> socket;
            {
                std::unique_lock lock(mutex_);
                (void)changed_.wait(lock, stop, [this] { return !outbox_.empty(); });
                if (stop.stop_requested()) return;
                socket = socket_;
                batch.swap(outbox_);
                queued_bytes_ = 0;
            }
            buffer.clear();
            for (const auto& frame : batch) buffer += frame;
            batch.clear();
            if (!socket) continue;  // disconnected meanwhile: retire() already counted them
            if (!socket->send_all(buffer)) {
                log(LogLevel::error, "Send to " + endpoint() + " failed: " + last_socket_error());
                socket->shutdown();  // let the reader notice and reconnect
            }
        }
    }

    const TcpOptions options_;
    const FrameHandler on_frame_;
    const LogFn log_;
    NetworkScope network_;

    // Shared between the MQL threads (connect/send/stop), the reader and the writer:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;  // connection state, outbox, kick or stop changed
    std::shared_ptr<Socket> socket_;       // the live connection; created and retired by the reader
    std::deque<std::string> outbox_;       // framed, waiting for the writer
    std::size_t queued_bytes_ = 0;
    bool kick_ = false;      // someone is waiting in connect(): skip the backoff
    bool stopping_ = false;
    std::atomic<bool> connected_{false};  // written under mutex_, read lock-free by connected()

    // feed()/reset() on the reader thread only; encode() is const and called from any thread.
    const std::unique_ptr<core::Framer> framer_;
    ReaderSink sink_;

    // Last: the threads start after, and are joined before, everything they touch.
    std::jthread io_thread_;
    std::jthread writer_thread_;
};

}  // namespace

std::unique_ptr<Transport> make_tcp_transport(TcpOptions options, Transport::FrameHandler on_frame,
                                              LogFn log) {
    return std::make_unique<TcpTransport>(std::move(options), std::move(on_frame), std::move(log));
}

}  // namespace mqlc::net
