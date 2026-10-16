#include <hog/replication/states.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace hog::replication {

namespace {

[[nodiscard]] std::unexpected<Error> unknown(std::string_view what, protocol::NetId id) {
    return make_error(ErrorCode::NotFound, std::format("{} {}, which isn't in the baseline", what,
                                                       std::to_underlying(id)));
}

} // namespace

Result<EntityStates> apply(const EntityStates& baseline, const protocol::Snapshot& snapshot) {
    EntityStates states = baseline;
    const auto find = [&states](protocol::NetId id) {
        const auto found = std::ranges::lower_bound(states, id, {}, &EntityState::id);
        return found != states.end() && found->id == id ? found : states.end();
    };
    for (const protocol::Changed& changed : snapshot.changed) {
        const auto entity = find(changed.id);
        if (entity == states.end()) {
            return unknown("changed entity", changed.id);
        }
        if (changed.moved_by) {
            entity->at += *changed.moved_by;
        }
    }
    for (const protocol::NetId id : snapshot.left) {
        const auto entity = find(id);
        if (entity == states.end()) {
            return unknown("entity leaving", id);
        }
        states.erase(entity);
    }
    for (const protocol::Entered& entered : snapshot.entered) {
        const auto place = std::ranges::lower_bound(states, entered.id, {}, &EntityState::id);
        if (place != states.end() && place->id == entered.id) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("entity {} entering is in the baseline already",
                                          std::to_underlying(entered.id)));
        }
        states.insert(place, {.id = entered.id, .at = entered.at, .name = entered.name});
    }
    return states;
}

} // namespace hog::replication
