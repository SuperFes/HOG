#pragma once

#include <hog/terrain/block_mesh.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace hog::client {

// Ground meshes to draw, each under its key in place of what was, and keys whose meshes are gone.
struct GroundChanges {
    struct Shown {
        std::uint64_t key = 0;
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water; // still, in lakes and pools
    };
    std::vector<Shown> shown;
    std::vector<std::uint64_t> gone;

    [[nodiscard]] bool empty() const noexcept { return shown.empty() && gone.empty(); }
};

} // namespace hog::client
