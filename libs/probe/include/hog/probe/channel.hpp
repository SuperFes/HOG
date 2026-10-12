#pragma once

#include <hog/core/result.hpp>
#include <hog/probe/messages.hpp>
#include <hog/probe/socket.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace hog::probe {

// Messages to and from one peer over a socket.
class Channel {
public:
    explicit Channel(Socket socket);

    [[nodiscard]] Result<void> send(const Message& message);
    // The next message, waiting up to `timeout` for it to arrive whole; none if it didn't. Fails
    // once the peer has closed or sent something unreadable.
    [[nodiscard]] Result<std::optional<Message>> receive(std::chrono::milliseconds timeout);

    void close() noexcept { socket_.close(); }
    [[nodiscard]] bool is_open() const noexcept { return socket_.is_open(); }

private:
    Socket socket_;
    FrameReader frames_;
    std::vector<std::byte> buffer_;
};

} // namespace hog::probe
