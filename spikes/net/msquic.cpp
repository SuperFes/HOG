// QUIC through msquic: unreliable messages as datagrams (RFC 9221), reliable ones framed on a
// bidirectional stream the client opens. msquic calls back on its own threads, so events are
// queued for poll. TLS needs a certificate; the clients here don't validate it.
#include "driver.hpp"

#include <msquic.h>

#include <cstdio>
#include <memory>
#include <mutex>

namespace {

using spike::Channel;

const QUIC_API_TABLE* api = nullptr;
constexpr char alpn_name[] = "hog";

struct Event {
    enum class Kind : std::uint8_t { connected, disconnected, received } kind;
    std::uint64_t id;
    std::vector<std::byte> bytes;
};

// A message waiting for msquic to finish sending it.
struct Outgoing {
    QUIC_BUFFER buffer{};
    std::vector<std::byte> bytes;
};

Outgoing* outgoing(std::span<const std::byte> bytes, bool framed) {
    auto* sending = new Outgoing;
    if (framed) {
        const auto size = static_cast<std::uint16_t>(bytes.size());
        sending->bytes.resize(2);
        std::memcpy(sending->bytes.data(), &size, 2);
    }
    sending->bytes.insert(sending->bytes.end(), bytes.begin(), bytes.end());
    sending->buffer.Length = static_cast<std::uint32_t>(sending->bytes.size());
    sending->buffer.Buffer = reinterpret_cast<std::uint8_t*>(sending->bytes.data());
    return sending;
}

class Quic;

struct Peer {
    Quic* owner = nullptr;
    std::uint64_t id = 0;
    HQUIC connection = nullptr;
    HQUIC stream = nullptr;
    std::vector<std::byte> partial; // of the stream's next message
};

class Quic {
public:
    bool open(bool server) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            return false;
        }
        const QUIC_REGISTRATION_CONFIG registration{"hog", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&registration, &registration_))) {
            return false;
        }
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs = 10'000;
        settings.IsSet.IdleTimeoutMs = TRUE;
        settings.DatagramReceiveEnabled = TRUE;
        settings.IsSet.DatagramReceiveEnabled = TRUE;
        if (server) {
            settings.PeerBidiStreamCount = 1;
            settings.IsSet.PeerBidiStreamCount = TRUE;
        }
        const QUIC_BUFFER alpn{sizeof alpn_name - 1,
                               reinterpret_cast<std::uint8_t*>(const_cast<char*>(alpn_name))};
        if (QUIC_FAILED(api->ConfigurationOpen(registration_, &alpn, 1, &settings, sizeof settings,
                                               nullptr, &configuration_))) {
            return false;
        }
        QUIC_CREDENTIAL_CONFIG credentials{};
        QUIC_CERTIFICATE_FILE files{"key.pem", "cert.pem"};
        if (server) {
            credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            credentials.CertificateFile = &files;
        } else {
            credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
            credentials.Flags =
                QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        }
        return QUIC_SUCCEEDED(api->ConfigurationLoadCredential(configuration_, &credentials));
    }

    template <class Handler>
    void poll(Handler& handler) {
        {
            const std::scoped_lock lock{mutex_};
            std::swap(events_, polled_);
        }
        for (Event& event : polled_) {
            switch (event.kind) {
            case Event::Kind::connected:
                handler.connected(event.id);
                break;
            case Event::Kind::disconnected:
                handler.disconnected(event.id);
                break;
            case Event::Kind::received:
                handler.received(event.id, event.bytes);
                break;
            }
        }
        polled_.clear();
    }

    void send(std::uint64_t id, Channel channel, std::span<const std::byte> bytes) {
        Peer* peer = nullptr;
        HQUIC stream = nullptr;
        {
            const std::scoped_lock lock{mutex_};
            const auto found = peers_.find(id);
            if (found == peers_.end()) {
                return;
            }
            peer = found->second;
            stream = peer->stream;
        }
        if (channel == Channel::unreliable) {
            Outgoing* sending = outgoing(bytes, false);
            if (QUIC_FAILED(api->DatagramSend(peer->connection, &sending->buffer, 1,
                                              QUIC_SEND_FLAG_NONE, sending))) {
                delete sending;
            }
        } else if (stream != nullptr) {
            Outgoing* sending = outgoing(bytes, true);
            if (QUIC_FAILED(
                    api->StreamSend(stream, &sending->buffer, 1, QUIC_SEND_FLAG_NONE, sending))) {
                delete sending;
            }
        }
    }

    void flush() {}

    void close() {
        if (api == nullptr) {
            return;
        }
        if (registration_ != nullptr) {
            api->RegistrationShutdown(registration_, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        }
        if (configuration_ != nullptr) {
            api->ConfigurationClose(configuration_);
        }
        if (registration_ != nullptr) {
            api->RegistrationClose(registration_);
        }
        MsQuicClose(api);
        api = nullptr;
    }

    // Called back from msquic's threads.
    void push(Event event) {
        const std::scoped_lock lock{mutex_};
        events_.push_back(std::move(event));
    }

    void add(Peer* peer) {
        const std::scoped_lock lock{mutex_};
        peers_[peer->id] = peer;
    }

    void set_stream(Peer* peer, HQUIC stream) {
        const std::scoped_lock lock{mutex_};
        peer->stream = stream;
    }

    void remove(Peer* peer) {
        const std::scoped_lock lock{mutex_};
        peers_.erase(peer->id);
    }

protected:
    static QUIC_STATUS QUIC_API on_stream(HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
        auto* peer = static_cast<Peer*>(context);
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            for (std::uint32_t at = 0; at < event->RECEIVE.BufferCount; ++at) {
                const QUIC_BUFFER& buffer = event->RECEIVE.Buffers[at];
                const auto* data = reinterpret_cast<const std::byte*>(buffer.Buffer);
                peer->partial.insert(peer->partial.end(), data, data + buffer.Length);
            }
            while (peer->partial.size() >= 2) {
                std::uint16_t size = 0;
                std::memcpy(&size, peer->partial.data(), 2);
                if (peer->partial.size() < 2U + size) {
                    break;
                }
                const auto begin = peer->partial.begin() + 2;
                peer->owner->push(
                    {Event::Kind::received, peer->id, std::vector<std::byte>(begin, begin + size)});
                peer->partial.erase(peer->partial.begin(), begin + size);
            }
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            delete static_cast<Outgoing*>(event->SEND_COMPLETE.ClientContext);
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            api->StreamClose(stream);
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    // What both ends do with a connection's events; the rest is left to `more`.
    static QUIC_STATUS common(HQUIC connection, Peer* peer, QUIC_CONNECTION_EVENT* event) {
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            const QUIC_BUFFER& buffer = *event->DATAGRAM_RECEIVED.Buffer;
            const auto* data = reinterpret_cast<const std::byte*>(buffer.Buffer);
            peer->owner->push({Event::Kind::received, peer->id,
                               std::vector<std::byte>(data, data + buffer.Length)});
            break;
        }
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State)) {
                delete static_cast<Outgoing*>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            peer->owner->remove(peer);
            peer->owner->push({Event::Kind::disconnected, peer->id, {}});
            api->ConnectionClose(connection);
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;

private:
    std::mutex mutex_;
    std::vector<Event> events_;
    std::vector<Event> polled_;
    std::unordered_map<std::uint64_t, Peer*> peers_;
};

class Server : public Quic {
public:
    bool start(std::uint16_t port) {
        if (!open(true)) {
            return false;
        }
        if (QUIC_FAILED(api->ListenerOpen(registration_, on_listener, this, &listener_))) {
            return false;
        }
        QUIC_ADDR address{};
        QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_INET);
        QuicAddrSetPort(&address, port);
        const QUIC_BUFFER alpn{sizeof alpn_name - 1,
                               reinterpret_cast<std::uint8_t*>(const_cast<char*>(alpn_name))};
        return QUIC_SUCCEEDED(api->ListenerStart(listener_, &alpn, 1, &address));
    }

    void stop() {
        if (listener_ != nullptr) {
            api->ListenerClose(listener_);
        }
        close();
    }

private:
    static QUIC_STATUS QUIC_API on_listener(HQUIC /*listener*/, void* context,
                                            QUIC_LISTENER_EVENT* event) {
        auto* server = static_cast<Server*>(context);
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
            return QUIC_STATUS_SUCCESS;
        }
        HQUIC connection = event->NEW_CONNECTION.Connection;
        auto* peer =
            new Peer{server, reinterpret_cast<std::uintptr_t>(connection), connection, nullptr, {}};
        api->SetCallbackHandler(connection, reinterpret_cast<void*>(on_connection), peer);
        return api->ConnectionSetConfiguration(connection, server->configuration_);
    }

    static QUIC_STATUS QUIC_API on_connection(HQUIC connection, void* context,
                                              QUIC_CONNECTION_EVENT* event) {
        auto* peer = static_cast<Peer*>(context);
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            peer->owner->add(peer);
            peer->owner->push({Event::Kind::connected, peer->id, {}});
            break;
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
                                    reinterpret_cast<void*>(on_stream), peer);
            peer->owner->set_stream(peer, event->PEER_STREAM_STARTED.Stream);
            break;
        default:
            return common(connection, peer, event);
        }
        return QUIC_STATUS_SUCCESS;
    }

    HQUIC listener_ = nullptr;
};

class Clients : public Quic {
public:
    bool start(int count, const std::string& host, std::uint16_t port) {
        if (!open(false)) {
            return false;
        }
        for (int index = 0; index < count; ++index) {
            auto peer = std::make_unique<Peer>();
            peer->owner = this;
            peer->id = static_cast<std::uint64_t>(index);
            if (QUIC_FAILED(api->ConnectionOpen(registration_, on_connection, peer.get(),
                                                &peer->connection))) {
                return false;
            }
            if (QUIC_FAILED(api->ConnectionStart(peer->connection, configuration_,
                                                 QUIC_ADDRESS_FAMILY_INET, host.c_str(), port))) {
                return false;
            }
            peers_.push_back(std::move(peer));
        }
        return true;
    }

    void stop() { close(); }

private:
    static QUIC_STATUS QUIC_API on_connection(HQUIC connection, void* context,
                                              QUIC_CONNECTION_EVENT* event) {
        auto* peer = static_cast<Peer*>(context);
        if (event->Type != QUIC_CONNECTION_EVENT_CONNECTED) {
            return common(connection, peer, event);
        }
        HQUIC stream = nullptr;
        if (QUIC_SUCCEEDED(api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE, on_stream, peer,
                                           &stream)) &&
            QUIC_SUCCEEDED(api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
            peer->owner->set_stream(peer, stream);
        }
        peer->owner->add(peer);
        peer->owner->push({Event::Kind::connected, peer->id, {}});
        return QUIC_STATUS_SUCCESS;
    }

    std::vector<std::unique_ptr<Peer>> peers_;
};

} // namespace

int main(int argc, char** argv) {
    return spike::run<Server, Clients>(argc, argv);
}
