// yojimbo over netcode: encrypted UDP whose connections need a connect token, signed with a key
// the login service and the servers share, and reliable-ordered and unreliable channels. Single
// threaded: nothing happens between calls, so packets go out only when SendPackets is called.
#include "driver.hpp"

#include <netcode.h>
#include <yojimbo.h>

#include <array>
#include <cstdio>
#include <memory>

namespace {

using spike::Channel;

constexpr std::uint64_t protocol_id = 0x484f47; // "HOG"
// Shared by the login service, which signs connect tokens, and the servers that check them.
constexpr std::array<std::uint8_t, yojimbo::KeyBytes> private_key = {
    0x60, 0x6a, 0xbe, 0x6e, 0xc9, 0x19, 0x10, 0xea, 0x9a, 0x65, 0x62, 0xf6, 0x6f, 0x2b, 0x30, 0xe4,
    0x43, 0x71, 0xd6, 0x2c, 0xd1, 0x99, 0x27, 0x26, 0x6b, 0x3c, 0x60, 0xf4, 0xb7, 0x15, 0xab, 0xa1};
constexpr std::size_t largest = 512;
constexpr double keep_alive = 0.05; // seconds between packets a client sends with nothing new

class Blob : public yojimbo::Message {
public:
    int size = 0;
    std::array<std::uint8_t, largest> data{};

    template <typename Stream>
    bool Serialize(Stream& stream) {
        serialize_int(stream, size, 0, static_cast<int>(largest));
        serialize_bytes(stream, data.data(), size);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

YOJIMBO_MESSAGE_FACTORY_START(Messages, 1);
YOJIMBO_DECLARE_MESSAGE_TYPE(0, Blob);
YOJIMBO_MESSAGE_FACTORY_FINISH();

struct Config : yojimbo::ClientServerConfig {
    Config() {
        protocolId = protocol_id;
        numChannels = 2;
        channel[static_cast<int>(Channel::unreliable)].type =
            yojimbo::CHANNEL_TYPE_UNRELIABLE_UNORDERED;
        channel[static_cast<int>(Channel::reliable)].type = yojimbo::CHANNEL_TYPE_RELIABLE_ORDERED;
        serverPerClientMemory = 2 * 1024 * 1024;
        maxConnectTokenLifetime = 45;
    }
};

class Adapter : public yojimbo::Adapter {
public:
    yojimbo::MessageFactory* CreateMessageFactory(yojimbo::Allocator& allocator) override {
        return YOJIMBO_NEW(allocator, Messages, allocator);
    }
    void OnServerClientConnected(int index) override { connected.push_back(index); }
    void OnServerClientDisconnected(int index) override { disconnected.push_back(index); }

    std::vector<int> connected;
    std::vector<int> disconnected;
};

void fill(Blob& blob, std::span<const std::byte> bytes) {
    blob.size = static_cast<int>(bytes.size());
    std::memcpy(blob.data.data(), bytes.data(), bytes.size());
}

std::span<const std::byte> bytes_of(const yojimbo::Message* message) {
    const auto* blob = static_cast<const Blob*>(message);
    return {reinterpret_cast<const std::byte*>(blob->data.data()),
            static_cast<std::size_t>(blob->size)};
}

class Server {
public:
    bool start(std::uint16_t port) {
        if (!InitializeYojimbo()) {
            return false;
        }
        yojimbo_log_level(YOJIMBO_LOG_LEVEL_ERROR);
        server_ = std::make_unique<yojimbo::Server>(
            yojimbo::GetDefaultAllocator(), private_key.data(), yojimbo::Address{"127.0.0.1", port},
            config_, adapter_, yojimbo_time());
        return server_->Start(yojimbo::MaxClients);
    }

    template <class Handler>
    void poll(Handler& handler) {
        server_->AdvanceTime(yojimbo_time());
        server_->ReceivePackets();
        for (const int index : adapter_.connected) {
            handler.connected(static_cast<std::uint64_t>(index));
        }
        for (const int index : adapter_.disconnected) {
            handler.disconnected(static_cast<std::uint64_t>(index));
        }
        adapter_.connected.clear();
        adapter_.disconnected.clear();
        for (int index = 0; index < yojimbo::MaxClients; ++index) {
            if (!server_->IsClientConnected(index)) {
                continue;
            }
            for (int channel = 0; channel < config_.numChannels; ++channel) {
                while (yojimbo::Message* message = server_->ReceiveMessage(index, channel)) {
                    handler.received(static_cast<std::uint64_t>(index), bytes_of(message));
                    server_->ReleaseMessage(index, message);
                }
            }
        }
    }

    void send(std::uint64_t peer, Channel channel, std::span<const std::byte> bytes) {
        const int index = static_cast<int>(peer);
        if (!server_->CanSendMessage(index, static_cast<int>(channel))) {
            return;
        }
        auto* blob = static_cast<Blob*>(server_->CreateMessage(index, 0));
        fill(*blob, bytes);
        server_->SendMessage(index, static_cast<int>(channel), blob);
    }

    // Once a tick, as the server's loop flushes.
    void flush() { server_->SendPackets(); }

    void stop() {
        server_->Stop();
        server_.reset();
        ShutdownYojimbo();
    }

private:
    Config config_;
    Adapter adapter_;
    std::unique_ptr<yojimbo::Server> server_;
};

class Clients {
public:
    bool start(int count, const std::string& host, std::uint16_t port) {
        if (!InitializeYojimbo()) {
            return false;
        }
        yojimbo_log_level(YOJIMBO_LOG_LEVEL_ERROR);
        const std::string address = host + ":" + std::to_string(port);
        const char* addresses[] = {address.c_str()};
        for (int index = 0; index < count; ++index) {
            auto client = std::make_unique<yojimbo::Client>(yojimbo::GetDefaultAllocator(),
                                                            yojimbo::Address{"0.0.0.0"}, config_,
                                                            adapter_, yojimbo_time());
            // As the login service would, once the player has signed in.
            std::array<std::uint8_t, yojimbo::ConnectTokenBytes> token{};
            std::array<std::uint8_t, NETCODE_USER_DATA_BYTES> user{};
            const auto id = static_cast<std::uint64_t>(index + 1);
            if (netcode_generate_connect_token(1, addresses, addresses, 45, 10, id, protocol_id,
                                               private_key.data(), user.data(),
                                               token.data()) != NETCODE_OK ||
                !client->Connect(id, token.data())) {
                return false;
            }
            clients_.push_back({std::move(client), false, false, 0.0});
        }
        return true;
    }

    template <class Handler>
    void poll(Handler& handler) {
        const double now = yojimbo_time();
        for (std::size_t index = 0; index < clients_.size(); ++index) {
            Entry& entry = clients_[index];
            entry.client->AdvanceTime(now);
            entry.client->ReceivePackets();
            const bool connected = entry.client->IsConnected();
            if (connected != entry.connected) {
                entry.connected = connected;
                connected ? handler.connected(index) : handler.disconnected(index);
            }
            if (!connected) {
                // Still connecting: its handshake packets go out as SendPackets is called.
                entry.client->SendPackets();
                continue;
            }
            for (int channel = 0; channel < config_.numChannels; ++channel) {
                while (yojimbo::Message* message = entry.client->ReceiveMessage(channel)) {
                    handler.received(index, bytes_of(message));
                    entry.client->ReleaseMessage(message);
                }
            }
        }
    }

    void send(std::uint64_t index, Channel channel, std::span<const std::byte> bytes) {
        Entry& entry = clients_[index];
        if (!entry.client->CanSendMessage(static_cast<int>(channel))) {
            return;
        }
        auto* blob = static_cast<Blob*>(entry.client->CreateMessage(0));
        fill(*blob, bytes);
        entry.client->SendMessage(static_cast<int>(channel), blob);
        entry.waiting = true;
    }

    // A packet from each client with something new, as a client's frame would send; and now and
    // then from the others, to carry acks.
    void flush() {
        const double now = yojimbo_time();
        for (Entry& entry : clients_) {
            if (entry.connected && (entry.waiting || now - entry.flushed >= keep_alive)) {
                entry.client->SendPackets();
                entry.waiting = false;
                entry.flushed = now;
            }
        }
    }

    void stop() {
        for (Entry& entry : clients_) {
            entry.client->Disconnect();
        }
        clients_.clear();
        ShutdownYojimbo();
    }

private:
    struct Entry {
        std::unique_ptr<yojimbo::Client> client;
        bool connected;
        bool waiting;
        double flushed;
    };
    Config config_;
    Adapter adapter_;
    std::vector<Entry> clients_;
};

} // namespace

int main(int argc, char** argv) {
    return spike::run<Server, Clients>(argc, argv);
}
