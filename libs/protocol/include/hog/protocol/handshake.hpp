#pragma once

#include <hog/core/time.hpp>
#include <hog/protocol/messages.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>

namespace hog::protocol {

// How long a client has, from connecting, to say hello.
inline constexpr std::chrono::seconds hello_within{5};

// What the server does about a connection while it shakes hands.
struct Answer {
    std::optional<Message> reply; // to send reliably, before closing if it closes
    bool close = false;
    std::string reason; // for closing, and for the log

    friend bool operator==(const Answer&, const Answer&) = default;
};

// The server's side of one connection's handshake: the first message must be a Hello of this
// version, within hello_within of connecting.
class Handshake {
public:
    explicit Handshake(MonotonicClock::time_point opened) noexcept
        : deadline_(opened + hello_within) {}

    [[nodiscard]] bool welcomed() const noexcept { return welcomed_; }

    // A message from the client, before it's welcomed.
    [[nodiscard]] Answer received(std::span<const std::byte> bytes);
    // What's done if it's still waiting for hello at `now`, once its time is up.
    [[nodiscard]] std::optional<Answer> waited(MonotonicClock::time_point now) const;

private:
    MonotonicClock::time_point deadline_;
    bool welcomed_ = false;
};

} // namespace hog::protocol
