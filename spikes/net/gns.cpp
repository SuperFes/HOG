// Valve's GameNetworkingSockets: encrypted, with reliable and unreliable messages on one
// connection. Without Steam there's no certificate authority here, so connections are encrypted
// but unauthenticated; ours would be the login service's tokens.
#include "driver.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <array>
#include <cstdio>

namespace {

using spike::Channel;

// Status changes arrive through a plain function during RunCallbacks; they're queued for poll.
struct Change {
    HSteamNetConnection connection;
    ESteamNetworkingConnectionState state;
};
std::vector<Change>* changes = nullptr;

void on_status(SteamNetConnectionStatusChangedCallback_t* status) {
    changes->push_back({status->m_hConn, status->m_info.m_eState});
}

void on_debug(ESteamNetworkingSocketsDebugOutputType /*type*/, const char* message) {
    std::fprintf(stderr, "%s\n", message);
}

bool initialise(std::vector<Change>& queue) {
    SteamDatagramErrMsg error{};
    if (!GameNetworkingSockets_Init(nullptr, error)) {
        std::fprintf(stderr, "%s\n", error);
        return false;
    }
    changes = &queue;
    ISteamNetworkingUtils* utils = SteamNetworkingUtils();
    utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, on_debug);
    utils->SetGlobalCallback_SteamNetConnectionStatusChanged(on_status);
    utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    return true;
}

int flags_for(Channel channel) {
    return channel == Channel::reliable ? k_nSteamNetworkingSend_ReliableNoNagle
                                        : k_nSteamNetworkingSend_UnreliableNoNagle;
}

template <class Handler, class Index>
void receive(HSteamNetPollGroup group, Handler& handler, Index index) {
    std::array<SteamNetworkingMessage_t*, 64> messages{};
    while (true) {
        const int got = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(
            group, messages.data(), static_cast<int>(messages.size()));
        if (got <= 0) {
            break;
        }
        for (int at = 0; at < got; ++at) {
            SteamNetworkingMessage_t* message = messages[static_cast<std::size_t>(at)];
            handler.received(index(message->m_conn),
                             std::span{static_cast<const std::byte*>(message->m_pData),
                                       static_cast<std::size_t>(message->m_cbSize)});
            message->Release();
        }
    }
}

class Server {
public:
    bool start(std::uint16_t port) {
        if (!initialise(changes_)) {
            return false;
        }
        SteamNetworkingIPAddr address{};
        address.Clear();
        address.m_port = port;
        listen_ = SteamNetworkingSockets()->CreateListenSocketIP(address, 0, nullptr);
        group_ = SteamNetworkingSockets()->CreatePollGroup();
        return listen_ != k_HSteamListenSocket_Invalid;
    }

    template <class Handler>
    void poll(Handler& handler) {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();
        for (const Change& change : changes_) {
            switch (change.state) {
            case k_ESteamNetworkingConnectionState_Connecting:
                sockets->AcceptConnection(change.connection);
                sockets->SetConnectionPollGroup(change.connection, group_);
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                handler.connected(change.connection);
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                handler.disconnected(change.connection);
                sockets->CloseConnection(change.connection, 0, nullptr, false);
                break;
            default:
                break;
            }
        }
        changes_.clear();
        receive(group_, handler,
                [](HSteamNetConnection connection) { return std::uint64_t{connection}; });
    }

    void send(std::uint64_t peer, Channel channel, std::span<const std::byte> bytes) {
        SteamNetworkingSockets()->SendMessageToConnection(
            static_cast<HSteamNetConnection>(peer), bytes.data(),
            static_cast<std::uint32_t>(bytes.size()), flags_for(channel), nullptr);
    }

    void flush() {}
    void stop() { GameNetworkingSockets_Kill(); }

private:
    std::vector<Change> changes_;
    HSteamListenSocket listen_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup group_ = k_HSteamNetPollGroup_Invalid;
};

class Clients {
public:
    bool start(int count, const std::string& host, std::uint16_t port) {
        if (!initialise(changes_)) {
            return false;
        }
        SteamNetworkingIPAddr address{};
        address.ParseString(host.c_str());
        address.m_port = port;
        group_ = SteamNetworkingSockets()->CreatePollGroup();
        for (int index = 0; index < count; ++index) {
            const HSteamNetConnection connection =
                SteamNetworkingSockets()->ConnectByIPAddress(address, 0, nullptr);
            SteamNetworkingSockets()->SetConnectionPollGroup(connection, group_);
            indices_[connection] = static_cast<std::uint64_t>(index);
            connections_.push_back(connection);
        }
        return true;
    }

    template <class Handler>
    void poll(Handler& handler) {
        SteamNetworkingSockets()->RunCallbacks();
        for (const Change& change : changes_) {
            if (change.state == k_ESteamNetworkingConnectionState_Connected) {
                handler.connected(indices_.at(change.connection));
            } else if (change.state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                       change.state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                handler.disconnected(indices_.at(change.connection));
            }
        }
        changes_.clear();
        receive(group_, handler,
                [this](HSteamNetConnection connection) { return indices_.at(connection); });
    }

    void send(std::uint64_t index, Channel channel, std::span<const std::byte> bytes) {
        SteamNetworkingSockets()->SendMessageToConnection(connections_[index], bytes.data(),
                                                          static_cast<std::uint32_t>(bytes.size()),
                                                          flags_for(channel), nullptr);
    }

    void flush() {}
    void stop() { GameNetworkingSockets_Kill(); }

private:
    std::vector<Change> changes_;
    HSteamNetPollGroup group_ = k_HSteamNetPollGroup_Invalid;
    std::vector<HSteamNetConnection> connections_;
    std::unordered_map<HSteamNetConnection, std::uint64_t> indices_;
};

} // namespace

int main(int argc, char** argv) {
    return spike::run<Server, Clients>(argc, argv);
}
