#pragma once

#include <hog/core/time.hpp>
#include <hog/login/keys.hpp>
#include <hog/login/token.hpp>
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
// version, within hello_within of connecting, and with a connect token the login service signed
// if the server knows its key.
class Handshake {
public:
    explicit Handshake(MonotonicClock::time_point opened,
                       std::optional<login::PublicKey> key = std::nullopt) noexcept
        : deadline_(opened + hello_within), key_(key) {}

    [[nodiscard]] bool welcomed() const noexcept { return welcomed_; }
    // Who the token let in; none when the server checks no tokens.
    [[nodiscard]] const std::optional<login::Token>& player() const noexcept { return player_; }

    // A message from the client, before it's welcomed, at `now` by the wall clock tokens keep.
    [[nodiscard]] Answer received(std::span<const std::byte> bytes, login::UnixTime now);
    // What's done if it's still waiting for hello at `now`, once its time is up.
    [[nodiscard]] std::optional<Answer> waited(MonotonicClock::time_point now) const;

private:
    MonotonicClock::time_point deadline_;
    std::optional<login::PublicKey> key_;
    std::optional<login::Token> player_;
    bool welcomed_ = false;
};

} // namespace hog::protocol
