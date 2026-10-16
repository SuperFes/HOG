#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>

struct SteamNetworkingMessage_t; // NOLINT(readability-identifier-naming): the library's name

namespace hog::net {

// One connection, as the server knows it. A client has only the one.
enum class ConnectionId : std::uint32_t {};

enum class Delivery : std::uint8_t {
    // May be lost or arrive out of order; for state where only the newest matters.
    Unreliable,
    // Arrives, in order with the other reliable messages, ahead of bulk.
    Reliable,
    // Arrives, in order with the other bulk messages, yielding to the two above.
    Bulk,
};

// The largest message either end may send.
inline constexpr std::size_t max_message_size = std::size_t{512} * 1024;

// A message that arrived, holding the library's buffer until it's destroyed.
class Message {
public:
    explicit Message(SteamNetworkingMessage_t* message) noexcept : message_(message) {}
    Message(Message&& other) noexcept : message_(std::exchange(other.message_, nullptr)) {}
    Message& operator=(Message&& other) noexcept;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    ~Message();

    [[nodiscard]] ConnectionId from() const noexcept;
    [[nodiscard]] Delivery delivery() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    SteamNetworkingMessage_t* message_;
};

struct Connected {
    ConnectionId connection;
};

struct Disconnected {
    ConnectionId connection;
    std::string reason;
};

// What a poll found, in order: connections made, then messages, then connections lost. A
// connection's messages all come after its Connected and before its Disconnected.
using Event = std::variant<Connected, Message, Disconnected>;

} // namespace hog::net
