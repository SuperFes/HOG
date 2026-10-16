#include "library.hpp"

#include <hog/net/client.hpp>

#include <format>
#include <optional>
#include <string>

namespace hog::net {

struct Client::State {
    std::unique_ptr<detail::Owner> owner;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    bool connected = false;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() {
        if (connection != k_HSteamNetConnection_Invalid) {
            detail::sockets().CloseConnection(connection, 0, nullptr, false);
        }
    }
};

Client::Client(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() = default;

Result<Client> Client::connect(std::string_view address) {
    SteamNetworkingIPAddr server{};
    if (!server.ParseString(std::string{address}.c_str()) || server.m_port == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} isn't an IP address and port", address));
    }
    auto owner = detail::Owner::create();
    if (!owner) {
        return std::unexpected(owner.error());
    }
    auto state = std::make_unique<State>();
    state->owner = std::move(*owner);
    const SteamNetworkingConfigValue_t options = detail::user_data(*state->owner);
    state->connection = detail::sockets().ConnectByIPAddress(server, 1, &options);
    if (state->connection == k_HSteamNetConnection_Invalid) {
        return make_error(ErrorCode::Io, std::format("couldn't connect to {}", address));
    }
    return Client{std::move(state)};
}

bool Client::is_connected() const noexcept {
    return state_->connected;
}

void Client::poll(std::vector<Event>& events) {
    events.clear();
    State& state = *state_;
    std::optional<std::string> lost;
    for (detail::Change& change : state.owner->changes()) {
        if (change.connection != state.connection) {
            continue;
        }
        if (change.state == k_ESteamNetworkingConnectionState_Connected) {
            detail::configure_lanes(state.connection);
            state.connected = true;
            events.emplace_back(Connected{ConnectionId{state.connection}});
        } else if (detail::closed(change.state)) {
            lost = std::move(change.reason);
        }
    }
    if (state.connection == k_HSteamNetConnection_Invalid) {
        return;
    }
    // What the server sent before the connection went is still delivered.
    detail::receive(
        [&](SteamNetworkingMessage_t** messages, int most) {
            return detail::sockets().ReceiveMessagesOnConnection(state.connection, messages, most);
        },
        events);
    if (lost) {
        detail::sockets().CloseConnection(state.connection, 0, nullptr, false);
        events.emplace_back(
            Disconnected{.connection = ConnectionId{state.connection}, .reason = std::move(*lost)});
        state.connection = k_HSteamNetConnection_Invalid;
        state.connected = false;
    }
}

Result<void> Client::send(Delivery delivery, std::span<const std::byte> bytes) {
    if (!state_->connected) {
        return make_error(ErrorCode::NotFound, "not connected");
    }
    return detail::send(state_->connection, delivery, bytes);
}

std::optional<Stats> Client::stats() const {
    if (!state_->connected) {
        return std::nullopt;
    }
    return detail::stats(state_->connection);
}

void Client::disconnect(std::string_view reason) {
    State& state = *state_;
    if (state.connection == k_HSteamNetConnection_Invalid) {
        return;
    }
    // Lingers until what's queued, such as the reason for closing, has gone.
    detail::sockets().CloseConnection(state.connection, k_ESteamNetConnectionEnd_App_Generic,
                                      std::string{reason}.c_str(), true);
    state.connection = k_HSteamNetConnection_Invalid;
    state.connected = false;
}

} // namespace hog::net
