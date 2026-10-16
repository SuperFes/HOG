#include "library.hpp"

#include <hog/net/server.hpp>

#include <format>
#include <string>
#include <unordered_set>

namespace hog::net {

struct Server::State {
    std::unique_ptr<detail::Owner> owner;
    HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
    // Made, and not yet lost or closed.
    std::unordered_set<HSteamNetConnection> connections;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() {
        for (const HSteamNetConnection connection : connections) {
            detail::sockets().CloseConnection(connection, 0, nullptr, false);
        }
        if (group != k_HSteamNetPollGroup_Invalid) {
            detail::sockets().DestroyPollGroup(group);
        }
        if (listen != k_HSteamListenSocket_Invalid) {
            detail::sockets().CloseListenSocket(listen);
        }
    }
};

Server::Server(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

Result<Server> Server::listen(std::uint16_t port) {
    if (port == 0) {
        return make_error(ErrorCode::InvalidArgument, "a server needs a port to listen on");
    }
    auto owner = detail::Owner::create();
    if (!owner) {
        return std::unexpected(owner.error());
    }
    auto state = std::make_unique<State>();
    state->owner = std::move(*owner);
    SteamNetworkingIPAddr address{};
    address.Clear();
    address.m_port = port;
    const SteamNetworkingConfigValue_t options = detail::user_data(*state->owner);
    state->listen = detail::sockets().CreateListenSocketIP(address, 1, &options);
    if (state->listen == k_HSteamListenSocket_Invalid) {
        return make_error(ErrorCode::Io, std::format("couldn't listen on port {}", port));
    }
    state->group = detail::sockets().CreatePollGroup();
    return Server{std::move(state)};
}

std::uint16_t Server::port() const {
    SteamNetworkingIPAddr address{};
    detail::sockets().GetListenSocketAddress(state_->listen, &address);
    return address.m_port;
}

void Server::poll(std::vector<Event>& events) {
    events.clear();
    ISteamNetworkingSockets& sockets = detail::sockets();
    std::vector<detail::Change> lost;
    for (detail::Change& change : state_->owner->changes()) {
        if (change.state == k_ESteamNetworkingConnectionState_Connecting) {
            if (sockets.AcceptConnection(change.connection) == k_EResultOK) {
                sockets.SetConnectionPollGroup(change.connection, state_->group);
            } else {
                sockets.CloseConnection(change.connection, 0, nullptr, false);
            }
        } else if (change.state == k_ESteamNetworkingConnectionState_Connected) {
            detail::configure_lanes(change.connection);
            state_->connections.insert(change.connection);
            events.emplace_back(Connected{ConnectionId{change.connection}});
        } else if (detail::closed(change.state)) {
            if (state_->connections.contains(change.connection)) {
                lost.push_back(std::move(change));
            } else {
                sockets.CloseConnection(change.connection, 0, nullptr, false);
            }
        }
    }
    // What a lost connection sent before it went is still delivered.
    detail::receive(
        [&](SteamNetworkingMessage_t** messages, int most) {
            return sockets.ReceiveMessagesOnPollGroup(state_->group, messages, most);
        },
        events);
    for (detail::Change& change : lost) {
        sockets.CloseConnection(change.connection, 0, nullptr, false);
        state_->connections.erase(change.connection);
        events.emplace_back(Disconnected{.connection = ConnectionId{change.connection},
                                         .reason = std::move(change.reason)});
    }
}

Result<void> Server::send(ConnectionId connection, Delivery delivery,
                          std::span<const std::byte> bytes) {
    const auto handle = static_cast<HSteamNetConnection>(connection);
    if (!state_->connections.contains(handle)) {
        return make_error(ErrorCode::NotFound, std::format("no connection {}", handle));
    }
    return detail::send(handle, delivery, bytes);
}

void Server::disconnect(ConnectionId connection, std::string_view reason) {
    const auto handle = static_cast<HSteamNetConnection>(connection);
    if (state_->connections.erase(handle) != 0) {
        // Lingers until what's queued, such as the reason for closing, has gone.
        detail::sockets().CloseConnection(handle, k_ESteamNetConnectionEnd_App_Generic,
                                          std::string{reason}.c_str(), true);
    }
}

} // namespace hog::net
