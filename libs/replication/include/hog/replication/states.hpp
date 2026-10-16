#pragma once

#include <hog/core/result.hpp>
#include <hog/protocol/messages.hpp>

#include <string>
#include <vector>

namespace hog::replication {

// An entity as a client holds it.
struct EntityState {
    protocol::NetId id{};
    protocol::Millimetres at{0};
    std::string name;

    friend bool operator==(const EntityState&, const EntityState&) = default;
};

// What a client holds of the world, in order of id.
using EntityStates = std::vector<EntityState>;

// `baseline` as `snapshot` changes it. Fails on a snapshot naming an entity entering that the
// baseline has, or one changed or leaving that it hasn't.
[[nodiscard]] Result<EntityStates> apply(const EntityStates& baseline,
                                         const protocol::Snapshot& snapshot);

} // namespace hog::replication
