#include <hog/core/assert.hpp>
#include <hog/server/players.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <glm/geometric.hpp>

#include <utility>

namespace hog::server {

std::string_view describe(Ruling ruling) noexcept {
    switch (ruling) {
    case Ruling::Taken:
        return "taken";
    case Ruling::Stale:
        return "stale";
    case Ruling::NotJoined:
        return "before joining";
    case Ruling::OutOfBounds:
        return "out of bounds";
    case Ruling::TooFast:
        return "too fast";
    case Ruling::UnderGround:
        return "under the ground";
    }
    return "?";
}

Players::Players(flecs::world& world, double radius, const replication::ViewSettings& view,
                 const MoveRules& rules) noexcept
    : world_(&world), nearest_(radius / 2.0), farthest_(radius * 2.0), view_(view), rules_(rules) {}

flecs::entity Players::join(net::ConnectionId connection, std::string name) {
    HOG_ASSERT(!joined_.contains(connection), "a connection joins once");
    const flecs::entity entity = world_->entity().set<Replicated>(
        {.id = protocol::NetId{next_id_++}, .name = std::move(name)});
    joined_.emplace(connection, Joined{.entity = entity, .view = replication::View{view_}});
    return entity;
}

MoveResult Players::move(net::ConnectionId connection, const protocol::Moved& moved,
                         MonotonicClock::time_point now, const physics::Space& space) {
    const auto found = joined_.find(connection);
    if (found == joined_.end()) {
        return {.ruling = Ruling::NotJoined, .correction = std::nullopt};
    }
    Joined& joined = found->second;
    if (moved.corrections != joined.corrections || moved.sequence <= joined.sequence) {
        return {.ruling = Ruling::Stale, .correction = std::nullopt};
    }
    joined.sequence = moved.sequence;

    const WorldPosition to = protocol::to_world(moved.at);
    const double from_centre = glm::length(to.metres());
    const bool inside = from_centre >= nearest_ && from_centre <= farthest_;
    if (!joined.check) {
        if (!inside) {
            return {.ruling = Ruling::OutOfBounds, .correction = std::nullopt};
        }
        joined.check.emplace(to, now, rules_);
        joined.entity.set<Position>({to});
        return {.ruling = Ruling::Taken, .correction = std::nullopt};
    }
    Ruling ruling = Ruling::OutOfBounds;
    if (inside) {
        switch (joined.check->judge(to, now, space)) {
        case Verdict::Taken:
            joined.entity.set<Position>({to});
            return {.ruling = Ruling::Taken, .correction = std::nullopt};
        case Verdict::TooFast:
            ruling = Ruling::TooFast;
            break;
        case Verdict::UnderGround:
            ruling = Ruling::UnderGround;
            break;
        }
    }
    ++joined.corrections;
    return {.ruling = ruling,
            .correction = protocol::Corrected{.count = joined.corrections,
                                              .at = protocol::to_millimetres(joined.check->at())}};
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

std::vector<WorldPosition> Players::placed() const {
    std::vector<WorldPosition> places;
    for (const auto& [connection, joined] : joined_) {
        if (joined.check) {
            places.push_back(joined.check->at());
        }
    }
    return places;
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
