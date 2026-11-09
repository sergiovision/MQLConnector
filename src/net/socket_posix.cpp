// POSIX (macOS/Linux) implementation of net/socket.h, so the core and transport tests run on a
// development Mac. The shipped DLL uses socket_winsock.cpp.
#include "net/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

namespace mqlc::net {
namespace {

constexpr auto kPollSlice = std::chrono::milliseconds(50);

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set per socket instead
#endif

int native(NativeSocket s) noexcept { return static_cast<int>(s); }

std::string error_text(int code) { return std::string(std::strerror(code)); }

void configure(int s) noexcept {
    const int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

bool set_blocking(int s, bool blocking) noexcept {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
}

int wait_one(int s, short events, std::chrono::milliseconds slice) noexcept {
    pollfd p{s, events, 0};
    int n = 0;
    do {
        n = poll(&p, 1, static_cast<int>(slice.count()));
    } while (n < 0 && errno == EINTR);
    return n;
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};

}  // namespace

NetworkScope::NetworkScope() noexcept : ok_(true) {}
NetworkScope::~NetworkScope() = default;

Socket Socket::connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout,
                       std::stop_token stop, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* raw = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &raw); rc != 0) {
        error = "resolve " + host + ": " + gai_strerror(rc);
        return {};
    }
    const std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    error = "no address for " + host;
    for (const addrinfo* ai = addresses.get(); ai != nullptr; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            error = error_text(errno);
            continue;
        }
        Socket candidate(static_cast<NativeSocket>(fd));
        set_blocking(fd, false);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
            error = error_text(errno);
            continue;
        }
        int ready = 0;
        while (ready == 0 && !stop.stop_requested()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero()) break;
            ready = wait_one(fd, POLLOUT, std::min(left, kPollSlice));
        }
        int so_error = 0;
        socklen_t length = sizeof so_error;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
        if (ready > 0 && so_error == 0) {
            set_blocking(fd, true);
            configure(fd);
            error.clear();
            return candidate;
        }
        error = stop.stop_requested() ? std::string("stopped")
                : so_error != 0       ? error_text(so_error)
                : ready < 0           ? error_text(errno)
                                      : "connect timed out";
        if (stop.stop_requested()) break;
    }
    return {};
}

std::ptrdiff_t Socket::receive(std::span<char> buffer) noexcept {
    ssize_t n = 0;
    do {
        n = recv(native(handle_), buffer.data(), buffer.size(), 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

bool Socket::send_all(std::span<const char> bytes) noexcept {
    while (!bytes.empty()) {
        const ssize_t n = send(native(handle_), bytes.data(), bytes.size(), kSendFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

void Socket::shutdown() noexcept {
    if (valid()) ::shutdown(native(handle_), SHUT_RDWR);
}

void Socket::abort() noexcept {
    if (!valid()) return;
    const linger hard{1, 0};
    setsockopt(native(handle_), SOL_SOCKET, SO_LINGER, &hard, sizeof hard);
    close();
}

void Socket::close() noexcept {
    if (valid()) ::close(native(release()));
}

Listener Listener::bind_loopback(std::uint16_t port, std::string& error) {
    Listener listener;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        error = error_text(errno);
        return listener;
    }
    Socket s(static_cast<NativeSocket>(fd));
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);  // rebind after a restart test
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
        listen(fd, SOMAXCONN) != 0) {
        error = error_text(errno);
        return listener;
    }
    socklen_t length = sizeof address;
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    listener.port_ = ntohs(address.sin_port);
    listener.socket_ = std::move(s);
    return listener;
}

Socket Listener::accept(std::chrono::milliseconds timeout) noexcept {
    const int fd = native(socket_.handle_);
    if (!socket_.valid() || wait_one(fd, POLLIN, timeout) <= 0) return {};
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) return {};
    configure(client);
    return Socket(static_cast<NativeSocket>(client));
}

std::string last_socket_error() { return error_text(errno); }

}  // namespace mqlc::net
