#pragma once

#include <hog/core/result.hpp>
#include <hog/net/events.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hog::net {

// One encrypted connection to a server.
class Client {
public:
    // Starts connecting to `address`, an IP address and port such as "127.0.0.1:7675" or
    // "[::1]:7675". Connected follows once the server accepts, or Disconnected if it doesn't.
    [[nodiscard]] static Result<Client> connect(std::string_view address);

    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Drops the connection without waiting for what's unsent.
    ~Client();

    [[nodiscard]] bool is_connected() const noexcept;

    // Replaces `events` with what happened since the last poll.
    void poll(std::vector<Event>& events);

    // Queues a message; fails unless connected, or if the message is too large.
    [[nodiscard]] Result<void> send(Delivery delivery, std::span<const std::byte> bytes);

    // Closes the connection, telling the server why. No Disconnected follows.
    void disconnect(std::string_view reason);

    struct State;

private:
    explicit Client(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

} // namespace hog::net
