#pragma once

#include <hog/core/result.hpp>
#include <hog/net/events.hpp>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace hog::net::detail {

// A change in a connection's state, as the library reported it.
struct Change {
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_None;
    std::string reason;
};

// Collects the changes to the connections a server or client owns. The library reports them
// while any owner runs its callbacks, maybe on another thread, so they wait here for their own
// owner's poll. Keeps the library initialised while it exists.
class Owner {
public:
    [[nodiscard]] static Result<std::unique_ptr<Owner>> create();
    ~Owner();
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    Owner(Owner&&) = delete;
    Owner& operator=(Owner&&) = delete;

    // Set as a connection's or listen socket's user data, which its connections inherit.
    [[nodiscard]] std::int64_t id() const noexcept { return id_; }

    // Runs the library's callbacks, then takes the changes to this owner's connections.
    [[nodiscard]] std::vector<Change> changes();

    void push(Change change);

private:
    explicit Owner(std::int64_t id) noexcept : id_(id) {}

    std::int64_t id_;
    std::mutex mutex_;
    std::vector<Change> changes_;
};

[[nodiscard]] inline ISteamNetworkingSockets& sockets() {
    return *SteamNetworkingSockets();
}

// Options every connection of `owner`'s starts with.
[[nodiscard]] SteamNetworkingConfigValue_t user_data(const Owner& owner);

// Gives a connected connection its lanes: reliable and unreliable first, bulk after.
void configure_lanes(HSteamNetConnection connection);

[[nodiscard]] Result<void> send(HSteamNetConnection connection, Delivery delivery,
                                std::span<const std::byte> bytes);

// Moves what arrived on a poll group or connection into `events`.
template <class Receive>
void receive(Receive receive_some, std::vector<Event>& events) {
    constexpr int batch = 64;
    std::array<SteamNetworkingMessage_t*, batch> messages{};
    while (true) {
        const int got = receive_some(messages.data(), batch);
        for (int index = 0; index < got; ++index) {
            events.emplace_back(Message{messages[static_cast<std::size_t>(index)]});
        }
        if (got < batch) {
            return;
        }
    }
}

[[nodiscard]] inline bool closed(ESteamNetworkingConnectionState state) noexcept {
    return state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
           state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
}

} // namespace hog::net::detail
