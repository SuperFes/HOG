#pragma once

#include <hog/core/result.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Blocking TCP, just enough for debug tools; the game's network library is chosen separately.
namespace hog::probe {

class Listener;

class Socket {
public:
    Socket() noexcept = default;
    Socket(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(const Socket&) = delete;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    [[nodiscard]] static Result<Socket> connect(const std::string& host, std::uint16_t port);

    [[nodiscard]] bool is_open() const noexcept;
    // Whether receive() would return without blocking, waiting up to `timeout`.
    [[nodiscard]] Result<bool> readable(std::chrono::milliseconds timeout) const;
    // What has arrived, up to `into`'s size, waiting for at least a byte; 0 once the peer closed.
    [[nodiscard]] Result<std::size_t> receive(std::span<std::byte> into) const;
    // Sends every byte, waiting as long as that takes.
    [[nodiscard]] Result<void> send(std::span<const std::byte> bytes) const;
    void close() noexcept;

private:
    friend class Listener;
    explicit Socket(std::uintptr_t handle) noexcept : handle_(handle) {}

    std::uintptr_t handle_ = invalid;
    static constexpr std::uintptr_t invalid = ~std::uintptr_t{0};
};

// Accepts connections on the loopback address only.
class Listener {
public:
    Listener() noexcept = default;
    Listener(const Listener&) = delete;
    Listener(Listener&& other) noexcept = default;
    Listener& operator=(const Listener&) = delete;
    Listener& operator=(Listener&& other) noexcept = default;
    ~Listener() = default;

    // Port 0 picks a free one.
    [[nodiscard]] static Result<Listener> open(std::uint16_t port);

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // Whether accept() would return without blocking, waiting up to `timeout`.
    [[nodiscard]] Result<bool> pending(std::chrono::milliseconds timeout) const;
    [[nodiscard]] Result<Socket> accept();

private:
    Socket socket_;
    std::uint16_t port_ = 0;
};

} // namespace hog::probe
