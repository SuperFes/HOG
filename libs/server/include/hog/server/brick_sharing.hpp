#pragma once

#include <hog/globe/brick.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/protocol/messages.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::server {

// Bricks of the voxel world told to players: every one held, as a player joins, and those
// changed since last told to everyone, in messages of at most most_bricks and about half of what
// a message may carry. The store must outlive it.
class BrickSharing {
public:
    explicit BrickSharing(const globe::VoxelStore& store) noexcept
        : store_{&store}, told_{store.revision()} {}

    // Every brick held, for a player joining.
    [[nodiscard]] std::vector<protocol::Bricks> all() const;
    // The bricks changed since last asked, those let go without bytes.
    [[nodiscard]] std::vector<protocol::Bricks> changed();

private:
    [[nodiscard]] std::vector<protocol::Bricks>
    batched(std::span<const globe::BrickSite> sites) const;

    const globe::VoxelStore* store_;
    std::uint64_t told_;
};

} // namespace hog::server
