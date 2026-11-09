// Winsock implementation of net/socket.h.
#include "net/socket.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <memory>

namespace mqlc::net {
namespace {

constexpr auto kPollSlice = std::chrono::milliseconds(50);  // how often a connect checks `stop`

SOCKET native(NativeSocket s) noexcept { return static_cast<SOCKET>(s); }

std::string error_text(int code) {
    char* message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&message), 0, nullptr);
    std::string text = "WSA error " + std::to_string(code);
    if (length > 0 && message != nullptr) {
        std::string_view body(message, length);
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' '))
            body.remove_suffix(1);
        text.append(": ").append(body);
    }
    LocalFree(message);
    return text;
}

bool set_blocking(SOCKET s, bool blocking) noexcept {
    u_long non_blocking = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &non_blocking) == 0;
}

void set_no_delay(SOCKET s) noexcept {
    const BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

/// select() on one socket without the FD_SET macros; returns > 0 ready, 0 timeout, < 0 error.
int wait_one(SOCKET s, bool for_write, std::chrono::milliseconds slice) noexcept {
    fd_set primary{};
    primary.fd_count = 1;
    primary.fd_array[0] = s;
    fd_set failed = primary;  // a failed connect is reported in the exception set on Windows
    timeval tv{};
    tv.tv_sec = static_cast<long>(slice.count() / 1000);
    tv.tv_usec = static_cast<long>((slice.count() % 1000) * 1000);
    const int n = for_write ? select(0, nullptr, &primary, &failed, &tv)
                            : select(0, &primary, nullptr, nullptr, &tv);
    return n;
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};

}  // namespace

NetworkScope::NetworkScope() noexcept {
    WSADATA data{};
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

NetworkScope::~NetworkScope() {
    if (ok_) WSACleanup();
}

Socket Socket::connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout,
                       std::stop_token stop, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* raw = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &raw); rc != 0) {
        error = "resolve " + host + ": " + error_text(rc);
        return {};
    }
    const std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    error = "no address for " + host;
    for (const addrinfo* ai = addresses.get(); ai != nullptr; ai = ai->ai_next) {
        Socket candidate(static_cast<NativeSocket>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)));
        if (!candidate.valid()) {
            error = error_text(WSAGetLastError());
            continue;
        }
        const SOCKET s = native(candidate.handle_);
        set_blocking(s, false);
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0 &&
            WSAGetLastError() != WSAEWOULDBLOCK) {
            error = error_text(WSAGetLastError());
            continue;
        }
        int ready = 0;
        while (ready == 0 && !stop.stop_requested()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero()) break;
            ready = wait_one(s, true, std::min(left, kPollSlice));
        }
        int so_error = 0;
        int length = sizeof so_error;
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &length);
        if (ready > 0 && so_error == 0) {
            set_blocking(s, true);
            set_no_delay(s);
            error.clear();
            return candidate;
        }
        error = stop.stop_requested() ? std::string("stopped")
                : so_error != 0       ? error_text(so_error)
                : ready < 0           ? error_text(WSAGetLastError())
                                      : "connect timed out";
        if (stop.stop_requested()) break;
    }
    return {};
}

std::ptrdiff_t Socket::receive(std::span<char> buffer) noexcept {
    const int length = static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX));
    const int n = recv(native(handle_), buffer.data(), length, 0);
    return n == SOCKET_ERROR ? -1 : n;
}

bool Socket::send_all(std::span<const char> bytes) noexcept {
    while (!bytes.empty()) {
        const int length = static_cast<int>(std::min<std::size_t>(bytes.size(), INT_MAX));
        const int n = send(native(handle_), bytes.data(), length, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

void Socket::shutdown() noexcept {
    if (!valid()) return;
    ::shutdown(native(handle_), SD_BOTH);
    // Winsock does not promise that shutdown() completes a recv/send already blocked in another
    // thread (POSIX does). Blocking calls on a default (overlapped) socket are overlapped I/O
    // underneath, so cancelling them ends the wait with WSA_OPERATION_ABORTED.
    CancelIoEx(reinterpret_cast<HANDLE>(native(handle_)), nullptr);
}

void Socket::abort() noexcept {
    if (!valid()) return;
    const linger hard{1, 0};
    setsockopt(native(handle_), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&hard), sizeof hard);
    close();
}

void Socket::close() noexcept {
    if (valid()) closesocket(native(release()));
}

Listener Listener::bind_loopback(std::uint16_t port, std::string& error) {
    Listener listener;
    Socket s(static_cast<NativeSocket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!s.valid()) {
        error = error_text(WSAGetLastError());
        return listener;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(native(s.handle_), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
        listen(native(s.handle_), SOMAXCONN) != 0) {
        error = error_text(WSAGetLastError());
        return listener;
    }
    int length = sizeof address;
    getsockname(native(s.handle_), reinterpret_cast<sockaddr*>(&address), &length);
    listener.port_ = ntohs(address.sin_port);
    listener.socket_ = std::move(s);
    return listener;
}

Socket Listener::accept(std::chrono::milliseconds timeout) noexcept {
    const SOCKET s = native(socket_.handle_);
    if (!socket_.valid() || wait_one(s, false, timeout) <= 0) return {};
    const SOCKET client = ::accept(s, nullptr, nullptr);
    if (client == INVALID_SOCKET) return {};
    set_no_delay(client);
    return Socket(static_cast<NativeSocket>(client));
}

std::string last_socket_error() { return error_text(WSAGetLastError()); }

}  // namespace mqlc::net
