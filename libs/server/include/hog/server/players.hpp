#pragma once

#include <hog/core/math.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/replication/view.hpp>
#include <hog/server/spatial_index.hpp>

#include <flecs.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hog::server {

// An entity others are told about: its id on the wire and its name.
struct Replicated {
    protocol::NetId id{};
    std::string name;
};

// The players' entities, by their connections, and what each is shown of those near them. They
// aren't persistent: a player who leaves is gone from the world.
class Players {
public:
    // Players may be from half to twice `radius` from the planet's centre. The world must
    // outlive this.
    Players(flecs::world& world, double radius,
            const replication::ViewSettings& view = {}) noexcept;

    // Makes a player's entity, with no Position until it first moves. Precondition: the
    // connection hasn't joined.
    flecs::entity join(net::ConnectionId connection, std::string name);
    // Until movement is the server's, it takes where the client says it is. False, changing
    // nothing, for a connection that hasn't joined or a place out of bounds.
    [[nodiscard]] bool move(net::ConnectionId connection, WorldPosition at);
    // Removes the player's entity; does nothing for a connection that hasn't joined.
    void leave(net::ConnectionId connection);
    // Does nothing for a connection that hasn't joined.
    void acknowledged(net::ConnectionId connection, std::uint32_t tick) noexcept;

    // Calls send(connection, snapshot) with each placed player's next snapshot, of the other
    // replicated entities `index` finds near them.
    template <typename Send>
        requires std::invocable<Send&, net::ConnectionId, const protocol::Snapshot&>
    void share(const SpatialIndex& index, Send send) {
        for (auto& [connection, joined] : joined_) {
            if (const auto snapshot = next(joined, index)) {
                send(connection, *snapshot);
            }
        }
    }

    [[nodiscard]] std::optional<flecs::entity> find(net::ConnectionId connection) const;
    [[nodiscard]] std::size_t size() const noexcept { return joined_.size(); }

private:
    struct Joined {
        flecs::entity entity;
        replication::View view;
    };

    // None for a player with no Position yet.
    [[nodiscard]] std::optional<protocol::Snapshot> next(Joined& joined, const SpatialIndex& index);

    flecs::world* world_;
    double nearest_;
    double farthest_;
    replication::ViewSettings view_;
    std::map<net::ConnectionId, Joined> joined_;
    // Reused from one snapshot to the next.
    std::vector<replication::Nearby> nearby_;
    // Ids aren't reused while the server runs, so a late snapshot never names the wrong entity.
    std::uint32_t next_id_ = 1;
};

} // namespace hog::server
