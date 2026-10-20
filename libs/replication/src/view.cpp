#include <hog/core/assert.hpp>
#include <hog/replication/view.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace hog::replication {

namespace {

// At most, for a gap between ids.
constexpr std::size_t id_bytes = 5;
// At most, for a snapshot's kind, tick, baseline and the lengths of its three lists.
constexpr std::size_t header_bytes = 2 + 4 + 5 + (3 * 5);

const EntityStates& nothing() {
    static const EntityStates none;
    return none;
}

std::size_t varint_bytes(std::uint64_t value) {
    std::size_t bytes = 1;
    for (; value > 0x7FU; value >>= 7U) {
        ++bytes;
    }
    return bytes;
}

std::size_t position_bytes(protocol::Millimetres at) {
    std::size_t bytes = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto bits = static_cast<std::uint64_t>(at[axis]);
        bytes += varint_bytes((bits << 1U) ^ (at[axis] < 0 ? ~std::uint64_t{0} : 0));
    }
    return bytes;
}

const EntityState* held(const EntityStates& states, protocol::NetId id) {
    const auto found = std::ranges::lower_bound(states, id, {}, &EntityState::id);
    return found != states.end() && found->id == id ? &*found : nullptr;
}

} // namespace

protocol::Snapshot View::next(std::span<const Nearby> nearby) {
    protocol::Snapshot snapshot{
        .tick = tick_ + 1, .baseline = {}, .entered = {}, .changed = {}, .left = {}};
    const EntityStates* baseline = &nothing();
    if (acknowledged_) {
        const auto found =
            std::ranges::find(sent_, *acknowledged_, [](const auto& entry) { return entry.first; });
        if (found != sent_.end()) {
            baseline = &found->second;
            snapshot.baseline = found->first;
        }
    }

    const double leaving = settings_.radius * settings_.leave_at;
    std::vector<const Nearby*> by_id;
    by_id.reserve(nearby.size());
    for (const Nearby& entity : nearby) {
        if (entity.distance <= leaving) {
            by_id.push_back(&entity);
        }
    }
    std::ranges::sort(by_id, {}, &Nearby::id);
    std::vector<const Nearby*> by_distance = by_id;
    std::ranges::stable_sort(by_distance, {}, &Nearby::distance);

    std::size_t used = header_bytes;
    // Those leaving go whatever the budget, as they cost little and mustn't linger.
    for (const EntityState& entity : *baseline) {
        const auto still = std::ranges::lower_bound(by_id, entity.id, {}, &Nearby::id);
        if (still == by_id.end() || (*still)->id != entity.id) {
            snapshot.left.push_back(entity.id);
            used += id_bytes;
        }
    }
    for (const Nearby* entity : by_distance) {
        if (const EntityState* was = held(*baseline, entity->id)) {
            if (was->at == entity->at) {
                continue;
            }
            const protocol::Millimetres moved_by = entity->at - was->at;
            const std::size_t cost = id_bytes + 1 + position_bytes(moved_by);
            if (used + cost <= settings_.budget) {
                snapshot.changed.push_back({.id = entity->id, .moved_by = moved_by});
                used += cost;
            }
        } else if (entity->distance <= settings_.radius) {
            const std::string_view name = entity->name.substr(0, protocol::most_name);
            const std::size_t cost = id_bytes + position_bytes(entity->at) + 1 + name.size();
            if (used + cost <= settings_.budget) {
                snapshot.entered.push_back(
                    {.id = entity->id, .at = entity->at, .name = std::string{name}});
                used += cost;
            }
        }
    }
    std::ranges::sort(snapshot.entered, {}, &protocol::Entered::id);
    std::ranges::sort(snapshot.changed, {}, &protocol::Changed::id);

    auto holding = replication::apply(*baseline, snapshot);
    HOG_ASSERT(holding.has_value(), "a view's snapshot must apply to its own baseline");
    tick_ = snapshot.tick;
    sent_.emplace_back(tick_, std::move(*holding));
    if (sent_.size() > kept) {
        sent_.pop_front();
    }
    return snapshot;
}

void View::acknowledged(std::uint32_t tick) noexcept {
    if (tick == 0 || tick > tick_ || (acknowledged_ && tick <= *acknowledged_)) {
        return;
    }
    acknowledged_ = tick;
}

const EntityStates& View::sent() const noexcept {
    return sent_.empty() ? nothing() : sent_.back().second;
}

} // namespace hog::replication
