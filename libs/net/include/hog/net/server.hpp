#pragma once

#include <hog/core/result.hpp>
#include <hog/net/events.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hog::net {

// Accepts encrypted connections on a UDP port and talks to each of them. Anyone may connect;
// telling who they are is the handshake's job.
class Server {
public:
    // Listens on `port`, which mustn't be 0, of every local address.
    [[nodiscard]] static Result<Server> listen(std::uint16_t port);

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    // Drops every connection without waiting for what's unsent.
    ~Server();

    [[nodiscard]] std::uint16_t port() const;

    // Replaces `events` with what happened since the last poll.
    void poll(std::vector<Event>& events);

    // Queues a message; fails if the connection is gone or the message too large.
    [[nodiscard]] Result<void> send(ConnectionId connection, Delivery delivery,
                                    std::span<const std::byte> bytes);

    // Closes a connection, telling the other end why. No Disconnected follows for it.
    void disconnect(ConnectionId connection, std::string_view reason);

    struct State;

private:
    explicit Server(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

} // namespace hog::net
