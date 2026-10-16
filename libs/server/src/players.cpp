#include <hog/core/assert.hpp>
#include <hog/server/players.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <glm/geometric.hpp>

#include <utility>

namespace hog::server {

Players::Players(flecs::world& world, double radius) noexcept
    : world_(&world), nearest_(radius / 2.0), farthest_(radius * 2.0) {}

flecs::entity Players::join(net::ConnectionId connection, std::string name) {
    HOG_ASSERT(!entities_.contains(connection), "a connection joins once");
    const flecs::entity entity = world_->entity().set<Replicated>(
        {.id = protocol::NetId{next_id_++}, .name = std::move(name)});
    entities_.emplace(connection, entity);
    return entity;
}

bool Players::move(net::ConnectionId connection, WorldPosition at) {
    const auto found = entities_.find(connection);
    if (found == entities_.end()) {
        return false;
    }
    const double from_centre = glm::length(at.metres());
    if (from_centre < nearest_ || from_centre > farthest_) {
        return false;
    }
    found->second.set<Position>({at});
    return true;
}

void Players::leave(net::ConnectionId connection) {
    const auto found = entities_.find(connection);
    if (found == entities_.end()) {
        return;
    }
    found->second.destruct();
    entities_.erase(found);
}

std::optional<flecs::entity> Players::find(net::ConnectionId connection) const {
    const auto found = entities_.find(connection);
    if (found == entities_.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace hog::server
