#pragma once

#include <hog/core/result.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/replication/states.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace hog::replication {

// What a client holds of the world, from the snapshots that reach it, whichever are lost or
// come out of order.
class Replica {
public:
    // Snapshots kept as baselines, as many as a view keeps.
    static constexpr std::size_t kept = 32;

    // True if the snapshot was applied, for the client to acknowledge; false if it's no newer
    // than one applied already, or coded against a baseline no longer kept. Fails if it doesn't
    // fit its baseline.
    [[nodiscard]] Result<bool> receive(const protocol::Snapshot& snapshot);

    // As of the newest snapshot applied.
    [[nodiscard]] const EntityStates& now() const noexcept;
    // None until one is.
    [[nodiscard]] std::optional<std::uint32_t> tick() const noexcept;

private:
    // Oldest first.
    std::deque<std::pair<std::uint32_t, EntityStates>> held_;
};

} // namespace hog::replication
