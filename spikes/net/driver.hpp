#pragma once

#include "scenario.hpp"

#include <cstdio>
#include <random>
#include <thread>
#include <unordered_map>

namespace spike {

// Runs the server's side of the scenario over any transport with start, poll, send and flush:
// a snapshot to every client at 20 Hz, chat echoed back on the tick it arrives by.
template <class Transport>
int run_server(Transport& transport, const Arguments& arguments) {
    if (!transport.start(arguments.port)) {
        std::fputs("server failed to start\n", stderr);
        return 1;
    }
    struct Peer {
        std::uint32_t snapshots = 0;
        std::uint32_t movements = 0;
        std::uint32_t first_movement = 0;
        std::uint32_t last_movement = 0;
        bool moved = false;
    };
    std::unordered_map<std::uint64_t, Peer> peers;
    int seen = 0;
    struct Handler {
        decltype(peers)& peers;
        int& seen;
        Transport& transport;
        void connected(std::uint64_t peer) {
            peers.try_emplace(peer);
            ++seen;
        }
        void disconnected(std::uint64_t peer) { (void)peer; }
        void received(std::uint64_t peer, std::span<const std::byte> bytes) {
            const auto header = decode(bytes);
            if (!header) {
                return;
            }
            Peer& state = peers[peer];
            if (header->kind == Kind::movement) {
                if (!state.moved) {
                    state.first_movement = header->sequence;
                    state.moved = true;
                }
                state.last_movement = std::max(state.last_movement, header->sequence);
                ++state.movements;
            } else if (header->kind == Kind::chat) {
                transport.send(peer, Channel::reliable, bytes);
            }
        }
    } handler{peers, seen, transport};

    const double cpu_before = cpu_seconds();
    const auto start = Clock::now();
    const auto end = start + std::chrono::duration<double>(arguments.seconds);
    // Ticks at the server's 20 Hz: what arrived is handled and answered, then snapshots go out.
    auto next_tick = start;
    while (Clock::now() < end) {
        transport.poll(handler);
        for (auto& [peer, state] : peers) {
            const auto bytes = encode(Kind::snapshot, state.snapshots++, snapshot_size);
            transport.send(peer, Channel::unreliable, bytes);
        }
        transport.flush();
        next_tick += snapshot_every;
        std::this_thread::sleep_until(next_tick);
    }
    const double cpu = cpu_seconds() - cpu_before;
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

    std::uint64_t movements = 0;
    std::uint64_t movements_sent = 0;
    for (const auto& [peer, state] : peers) {
        movements += state.movements;
        if (state.moved) {
            movements_sent += state.last_movement - state.first_movement + 1;
        }
    }
    std::printf("{\"role\":\"server\",\"clients\":%d,\"movement_delivered\":%.4f,"
                "\"cpu_percent\":%.2f}\n",
                seen,
                movements_sent > 0
                    ? static_cast<double>(movements) / static_cast<double>(movements_sent)
                    : 0.0,
                100.0 * cpu / seconds);
    transport.stop();
    return 0;
}

// Runs the clients' side: movement at 30 Hz and a chat line a second from each, for the given
// time, then a few seconds more to let what's in flight arrive.
template <class Transport>
int run_clients(Transport& transport, const Arguments& arguments) {
    constexpr auto drain = std::chrono::seconds{3};
    const auto count = static_cast<std::size_t>(arguments.clients);
    struct Client {
        bool connected = false;
        Clock::time_point next_movement;
        Clock::time_point next_chat;
        std::uint32_t movements = 0;
        std::uint32_t chats = 0;
        std::uint32_t chat_expected = 0;
        std::uint32_t chats_back = 0;
        std::uint32_t out_of_order = 0;
        std::uint32_t snapshots = 0;
        std::uint32_t first_snapshot = 0;
        std::uint32_t last_snapshot = 0;
    };
    std::vector<Client> clients(count);
    Samples connect_ms;
    Samples chat_ms;
    Samples snapshot_ms;
    std::mt19937 random{1234};
    std::uniform_int_distribution<int> phase_us{0, 33'333};

    const auto start = Clock::now();
    struct Handler {
        std::vector<Client>& clients;
        Samples& connect_ms;
        Samples& chat_ms;
        Samples& snapshot_ms;
        Clock::time_point start;
        std::mt19937& random;
        std::uniform_int_distribution<int>& phase_us;
        void connected(std::uint64_t index) {
            Client& client = clients[index];
            client.connected = true;
            const auto now = Clock::now();
            connect_ms.add(std::chrono::duration<double, std::milli>(now - start).count());
            // Spread clients over the tick, as players' clocks would be.
            client.next_movement = now + std::chrono::microseconds{phase_us(random)};
            client.next_chat = client.next_movement;
        }
        void disconnected(std::uint64_t index) { clients[index].connected = false; }
        void received(std::uint64_t index, std::span<const std::byte> bytes) {
            const auto header = decode(bytes);
            if (!header) {
                return;
            }
            Client& client = clients[index];
            if (header->kind == Kind::snapshot) {
                snapshot_ms.add(milliseconds_since(header->sent_ns));
                if (client.snapshots == 0) {
                    client.first_snapshot = header->sequence;
                }
                client.last_snapshot = std::max(client.last_snapshot, header->sequence);
                ++client.snapshots;
            } else if (header->kind == Kind::chat) {
                chat_ms.add(milliseconds_since(header->sent_ns));
                if (header->sequence != client.chat_expected) {
                    ++client.out_of_order;
                }
                client.chat_expected = header->sequence + 1;
                ++client.chats_back;
            }
        }
    } handler{clients, connect_ms, chat_ms, snapshot_ms, start, random, phase_us};

    if (!transport.start(arguments.clients, arguments.host, arguments.port)) {
        std::fputs("clients failed to start\n", stderr);
        return 1;
    }
    const auto sending_until = start + std::chrono::duration<double>(arguments.seconds);
    const auto end = sending_until + drain;
    // Wire bytes are counted while every client is connected and sending.
    std::optional<std::pair<Clock::time_point, std::uint64_t>> counting;
    std::optional<std::pair<Clock::time_point, std::uint64_t>> counted;
    while (Clock::now() < end) {
        transport.poll(handler);
        const auto now = Clock::now();
        if (!counting && connect_ms.count() == count) {
            counting.emplace(now, loopback_bytes());
        }
        if (counting && !counted && now >= sending_until) {
            counted.emplace(now, loopback_bytes());
        }
        if (now < sending_until) {
            for (std::size_t index = 0; index < count; ++index) {
                Client& client = clients[index];
                if (!client.connected) {
                    continue;
                }
                if (now >= client.next_movement) {
                    transport.send(index, Channel::unreliable,
                                   encode(Kind::movement, client.movements++, movement_size));
                    client.next_movement += movement_every;
                }
                if (now >= client.next_chat) {
                    transport.send(index, Channel::reliable,
                                   encode(Kind::chat, client.chats++, chat_size));
                    client.next_chat += chat_every;
                }
            }
        }
        transport.flush();
        std::this_thread::sleep_for(poll_every);
    }

    int connected = 0;
    std::uint64_t chats = 0;
    std::uint64_t chats_back = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t snapshots_sent = 0;
    for (const Client& client : clients) {
        connected += client.connected ? 1 : 0;
        chats += client.chats;
        chats_back += client.chats_back;
        out_of_order += client.out_of_order;
        snapshots += client.snapshots;
        if (client.snapshots > 0) {
            snapshots_sent += client.last_snapshot - client.first_snapshot + 1;
        }
    }
    std::printf(
        "{\"role\":\"clients\",\"connected\":%d,\"of\":%d,\"connect_ms_p50\":%.1f,"
        "\"connect_ms_max\":%.1f,\"chat_back\":%.4f,\"chat_out_of_order\":%llu,"
        "\"chat_rtt_ms_p50\":%.1f,\"chat_rtt_ms_p99\":%.1f,\"chat_rtt_ms_max\":%.1f,"
        "\"snapshot_delivered\":%.4f,\"snapshot_age_ms_p50\":%.1f,\"snapshot_age_ms_p99\":%.1f,"
        "\"wire_bytes_per_client_second\":%.0f}\n",
        connected, arguments.clients, connect_ms.percentile(0.5), connect_ms.percentile(1.0),
        chats > 0 ? static_cast<double>(chats_back) / static_cast<double>(chats) : 0.0,
        static_cast<unsigned long long>(out_of_order), chat_ms.percentile(0.5),
        chat_ms.percentile(0.99), chat_ms.percentile(1.0),
        snapshots_sent > 0 ? static_cast<double>(snapshots) / static_cast<double>(snapshots_sent)
                           : 0.0,
        snapshot_ms.percentile(0.5), snapshot_ms.percentile(0.99),
        counted
            ? static_cast<double>(counted->second - counting->second) / static_cast<double>(count) /
                  std::chrono::duration<double>(counted->first - counting->first).count()
            : 0.0);
    transport.stop();
    return 0;
}

template <class Server, class Clients>
int run(int argc, char** argv) {
    const Arguments arguments = parse(argc, argv);
    if (arguments.server) {
        Server server;
        return run_server(server, arguments);
    }
    Clients clients;
    return run_clients(clients, arguments);
}

} // namespace spike
