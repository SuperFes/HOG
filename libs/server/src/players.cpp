#include <hog/core/assert.hpp>
#include <hog/server/players.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <glm/geometric.hpp>

#include <utility>

namespace hog::server {

Players::Players(flecs::world& world, double radius, const replication::ViewSettings& view) noexcept
    : world_(&world), nearest_(radius / 2.0), farthest_(radius * 2.0), view_(view) {}

flecs::entity Players::join(net::ConnectionId connection, std::string name) {
    HOG_ASSERT(!joined_.contains(connection), "a connection joins once");
    const flecs::entity entity = world_->entity().set<Replicated>(
        {.id = protocol::NetId{next_id_++}, .name = std::move(name)});
    joined_.emplace(connection, Joined{.entity = entity, .view = replication::View{view_}});
    return entity;
}

bool Players::move(net::ConnectionId connection, WorldPosition at) {
    const auto found = joined_.find(connection);
    if (found == joined_.end()) {
        return false;
    }
    const double from_centre = glm::length(at.metres());
    if (from_centre < nearest_ || from_centre > farthest_) {
        return false;
    }
    found->second.entity.set<Position>({at});
    return true;
}

void Players::leave(net::ConnectionId connection) {
    const auto found = joined_.find(connection);
    if (found == joined_.end()) {
        return;
    }
    found->second.entity.destruct();
    joined_.erase(found);
}

void Players::acknowledged(net::ConnectionId connection, std::uint32_t tick) noexcept {
    if (const auto found = joined_.find(connection); found != joined_.end()) {
        found->second.view.acknowledged(tick);
    }
}

std::optional<flecs::entity> Players::find(net::ConnectionId connection) const {
    const auto found = joined_.find(connection);
    if (found == joined_.end()) {
        return std::nullopt;
    }
    return found->second.entity;
}

std::optional<protocol::Snapshot> Players::next(Joined& joined, const SpatialIndex& index) {
    const auto* placed = joined.entity.try_get<Position>();
    if (placed == nullptr) {
        return std::nullopt;
    }
    const WorldPosition centre = placed->at;
    nearby_.clear();
    index.for_each_near(
        centre, view_.radius * view_.leave_at, [&](const SpatialIndex::Entry& entry) {
            if (entry.id == joined.entity.id()) {
                return;
            }
            const auto* replicated = flecs::entity{*world_, entry.id}.try_get<Replicated>();
            if (replicated == nullptr) {
                return;
            }
            nearby_.push_back({.id = replicated->id,
                               .at = protocol::to_millimetres(entry.position),
                               .name = replicated->name,
                               .distance = glm::length(entry.position - centre)});
        });
    return joined.view.next(nearby_);
}

} // namespace hog::server
