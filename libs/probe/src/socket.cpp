#include <hog/probe/socket.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace hog::probe {

namespace {

std::string describe(int error) {
    return std::system_category().message(error);
}

#ifdef _WIN32
using Native = SOCKET;
using Length = int;
using Count = int;
constexpr int send_flags = 0;

int last_error() noexcept {
    return WSAGetLastError();
}
bool interrupted(int /*error*/) noexcept {
    return false;
}
int close_native(Native native) noexcept {
    return closesocket(native);
}
int poll_native(pollfd* fds, unsigned long count, int timeout) noexcept {
    return WSAPoll(fds, count, timeout);
}

// Winsock needs starting once per process before any other call.
Result<void> start_sockets() {
    static const int started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (started != 0) {
        return make_error(ErrorCode::Io, std::format("can't start sockets: {}", describe(started)));
    }
    return {};
}
#else
using Native = int;
using Length = socklen_t;
using Count = std::size_t;
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

int last_error() noexcept {
    return errno;
}
bool interrupted(int error) noexcept {
    return error == EINTR;
}
int close_native(Native native) noexcept {
    return ::close(native);
}
int poll_native(pollfd* fds, nfds_t count, int timeout) noexcept {
    return ::poll(fds, count, timeout);
}

Result<void> start_sockets() {
    return {};
}
#endif

Native native_of(std::uintptr_t handle) noexcept {
    return static_cast<Native>(handle);
}

// The sockets API takes buffers as char and IPv4 addresses as its generic sockaddr.
const char* chars(const void* bytes) noexcept {
    return static_cast<const char*>(bytes);
}
char* chars(void* bytes) noexcept {
    return static_cast<char*>(bytes);
}
sockaddr* generic(sockaddr_in& address) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&address);
}

std::unexpected<Error> failure(std::string_view doing) {
    return make_error(ErrorCode::Io, std::format("{}: {}", doing, describe(last_error())));
}

// Small debug messages go out at once rather than waiting to fill a packet.
void tune(Native native) noexcept {
    int on = 1;
    setsockopt(native, IPPROTO_TCP, TCP_NODELAY, chars(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
    setsockopt(native, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

Result<bool> wait_readable(std::uintptr_t handle, std::chrono::milliseconds timeout) {
    pollfd poll{.fd = native_of(handle), .events = POLLIN, .revents = 0};
    const auto wait = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(
        timeout.count(), 0, std::numeric_limits<int>::max()));
    for (;;) {
        const int ready = poll_native(&poll, 1, wait);
        if (ready >= 0) {
            return ready > 0;
        }
        if (!interrupted(last_error())) {
            return failure("can't wait on socket");
        }
    }
}

struct AddressList {
    void operator()(addrinfo* list) const noexcept { freeaddrinfo(list); }
};

} // namespace

Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalid)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalid);
    }
    return *this;
}

Socket::~Socket() {
    close();
}

Result<Socket> Socket::connect(const std::string& host, std::uint16_t port) {
    if (auto started = start_sockets(); !started) {
        return std::unexpected(std::move(started.error()));
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(port);
    if (const int error = getaddrinfo(host.c_str(), service.c_str(), &hints, &found); error != 0) {
        return make_error(ErrorCode::NotFound,
                          std::format("can't find {}: {}", host, gai_strerror(error)));
    }
    const std::unique_ptr<addrinfo, AddressList> addresses{found};
    std::string last = "no addresses";
    for (const addrinfo* address = found; address != nullptr; address = address->ai_next) {
        const Native native =
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        Socket socket{static_cast<std::uintptr_t>(native)};
        if (!socket.is_open()) {
            last = describe(last_error());
            continue;
        }
        if (::connect(native, address->ai_addr, static_cast<Length>(address->ai_addrlen)) == 0) {
            tune(native);
            return socket;
        }
        last = describe(last_error());
    }
    return make_error(ErrorCode::Io, std::format("can't connect to {}:{}: {}", host, port, last));
}

bool Socket::is_open() const noexcept {
#ifdef _WIN32
    return native_of(handle_) != INVALID_SOCKET;
#else
    return native_of(handle_) >= 0;
#endif
}

Result<bool> Socket::readable(std::chrono::milliseconds timeout) const {
    if (!is_open()) {
        return make_error(ErrorCode::InvalidArgument, "socket is closed");
    }
    return wait_readable(handle_, timeout);
}

Result<std::size_t> Socket::receive(std::span<std::byte> into) const {
    if (!is_open()) {
        return make_error(ErrorCode::InvalidArgument, "socket is closed");
    }
    const auto most = static_cast<Count>(
        std::min(into.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    for (;;) {
        const auto got = ::recv(native_of(handle_), chars(into.data()), most, 0);
        if (got >= 0) {
            return static_cast<std::size_t>(got);
        }
        if (!interrupted(last_error())) {
            return failure("can't receive");
        }
    }
}

Result<void> Socket::send(std::span<const std::byte> bytes) const {
    if (!is_open()) {
        return make_error(ErrorCode::InvalidArgument, "socket is closed");
    }
    while (!bytes.empty()) {
        const auto most = static_cast<Count>(
            std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const auto sent = ::send(native_of(handle_), chars(bytes.data()), most, send_flags);
        if (sent < 0) {
            if (interrupted(last_error())) {
                continue;
            }
            return failure("can't send");
        }
        bytes = bytes.subspan(static_cast<std::size_t>(sent));
    }
    return {};
}

void Socket::close() noexcept {
    if (is_open()) {
        close_native(native_of(handle_));
        handle_ = invalid;
    }
}

Result<Listener> Listener::open(std::uint16_t port) {
    if (auto started = start_sockets(); !started) {
        return std::unexpected(std::move(started.error()));
    }
    Listener listener;
    const Native native = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    listener.socket_ = Socket{static_cast<std::uintptr_t>(native)};
    if (!listener.socket_.is_open()) {
        return failure("can't make a socket");
    }
#ifndef _WIN32
    // A restarted server can take its port back while old connections wind down.
    int on = 1;
    setsockopt(native, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(native, generic(address), sizeof(address)) != 0) {
        return failure(std::format("can't listen on port {}", port));
    }
    if (::listen(native, SOMAXCONN) != 0) {
        return failure(std::format("can't listen on port {}", port));
    }
    sockaddr_in bound{};
    Length length = sizeof(bound);
    if (getsockname(native, generic(bound), &length) != 0) {
        return failure("can't read the listening port");
    }
    listener.port_ = ntohs(bound.sin_port);
    return listener;
}

Result<bool> Listener::pending(std::chrono::milliseconds timeout) const {
    return socket_.readable(timeout);
}

Result<Socket> Listener::accept() {
    if (!socket_.is_open()) {
        return make_error(ErrorCode::InvalidArgument, "listener is closed");
    }
    for (;;) {
        const Native native = ::accept(native_of(socket_.handle_), nullptr, nullptr);
        Socket socket{static_cast<std::uintptr_t>(native)};
        if (socket.is_open()) {
            tune(native);
            return socket;
        }
        if (!interrupted(last_error())) {
            return failure("can't accept");
        }
    }
}

} // namespace hog::probe
