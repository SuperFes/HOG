#include <hog/replication/timeline.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace hog::replication {

namespace {

// In snapshot intervals; whole clock ticks divide exactly where seconds wouldn't.
double ticks(MonotonicClock::duration span) {
    constexpr auto interval =
        std::chrono::duration_cast<MonotonicClock::duration>(protocol::snapshot_interval);
    return static_cast<double>(span.count()) / static_cast<double>(interval.count());
}

const EntityState* find(const EntityStates& states, protocol::NetId id) {
    const auto found = std::ranges::lower_bound(states, id, {}, &EntityState::id);
    return found != states.end() && found->id == id ? &*found : nullptr;
}

// Those in `members`, `along` the way from where `from` has them to where `to` does; where
// `members` has them, if either hasn't.
std::vector<Shown> blend(const EntityStates& members, const EntityStates& from,
                         const EntityStates& to, double along) {
    std::vector<Shown> shown;
    shown.reserve(members.size());
    for (const EntityState& member : members) {
        WorldPosition at = protocol::to_world(member.at);
        const EntityState* start = find(from, member.id);
        const EntityState* end = find(to, member.id);
        if (start != nullptr && end != nullptr) {
            const WorldPosition first = protocol::to_world(start->at);
            at = first + ((protocol::to_world(end->at) - first) * along);
        }
        shown.push_back({.id = member.id, .at = at, .name = member.name});
    }
    return shown;
}

} // namespace

void Timeline::add(std::uint32_t tick, MonotonicClock::time_point arrived, EntityStates states) {
    if (!held_.empty() && tick <= held_.back().tick) {
        return;
    }
    held_.push_back({.tick = tick, .states = std::move(states)});
    if (held_.size() > kept) {
        held_.pop_front();
    }
    const MonotonicClock::time_point started =
        arrived -
        std::chrono::duration_cast<MonotonicClock::duration>(protocol::snapshot_interval * tick);
    // One so late it would already be past showing means the server stalled, or the way here
    // changed: the clock starts again from it.
    if (!starts_.empty() &&
        started - std::ranges::min(starts_) > settings_.delay + settings_.reach) {
        starts_.clear();
    }
    starts_.push_back(started);
    if (starts_.size() > fitted) {
        starts_.pop_front();
    }
}

std::vector<Shown> Timeline::at(MonotonicClock::time_point now) const {
    if (held_.empty()) {
        return {};
    }
    const MonotonicClock::time_point start = std::ranges::min(starts_);
    const double tick = ticks(now - settings_.delay - start);
    // The first snapshot after the moment shown.
    const auto after =
        std::ranges::find_if(held_, [tick](const Held& held) { return held.tick > tick; });
    if (after == held_.begin()) {
        const EntityStates& oldest = held_.front().states;
        return blend(oldest, oldest, oldest, 0.0);
    }
    const auto before = std::prev(after);
    if (after != held_.end()) {
        const double along = (tick - before->tick) / (after->tick - before->tick);
        return blend(before->states, before->states, after->states, along);
    }
    if (before == held_.begin()) {
        return blend(before->states, before->states, before->states, 0.0);
    }
    const auto previous = std::prev(before);
    const double over = std::min(tick - before->tick, ticks(settings_.reach));
    const double along = 1.0 + (over / (before->tick - previous->tick));
    return blend(before->states, previous->states, before->states, along);
}

} // namespace hog::replication
