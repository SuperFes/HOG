#pragma once

#include <hog/core/math.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace hog::server {

// An entity others are told about: its id on the wire and its name.
struct Replicated {
    protocol::NetId id{};
    std::string name;
};

// The players' entities, by their connections. They aren't persistent: a player who leaves is
// gone from the world.
class Players {
public:
    // Players may be from half to twice `radius` from the planet's centre. The world must
    // outlive this.
    Players(flecs::world& world, double radius) noexcept;

    // Makes a player's entity, with no Position until it first moves. Precondition: the
    // connection hasn't joined.
    flecs::entity join(net::ConnectionId connection, std::string name);
    // Until movement is the server's, it takes where the client says it is. False, changing
    // nothing, for a connection that hasn't joined or a place out of bounds.
    [[nodiscard]] bool move(net::ConnectionId connection, WorldPosition at);
    // Removes the player's entity; does nothing for a connection that hasn't joined.
    void leave(net::ConnectionId connection);

    [[nodiscard]] std::optional<flecs::entity> find(net::ConnectionId connection) const;
    [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }

private:
    flecs::world* world_;
    double nearest_;
    double farthest_;
    std::map<net::ConnectionId, flecs::entity> entities_;
    // Ids aren't reused while the server runs, so a late snapshot never names the wrong entity.
    std::uint32_t next_id_ = 1;
};

} // namespace hog::server
