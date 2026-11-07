#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace hog::globe {

// The tile columns a client holds as they're sent it: those at their own level in its volume,
// coarser ones beside it, each column at one level at a time.
class ColumnCache {
public:
    // A tile column held coarser than its own level.
    struct Coarse {
        std::uint8_t level = 0;
        TileColumn column;
    };

    explicit ColumnCache(Volume& volume) : volume_{&volume} {}

    // Holds the tile column at (`face`, `i`, `j`), counted in tiles, at `level`, in place of what
    // was held of it; no bytes is nothing made there. Fails, changing nothing, on bytes that
    // aren't one at that level.
    Result<void> take(std::uint8_t face, std::uint32_t i, std::uint32_t j, std::uint8_t level,
                      std::span<const std::byte> bytes);
    // The tile column at `corner` where it's held coarser than its own level.
    [[nodiscard]] const Coarse* coarse(Site corner) const;
    // Lets go every tile column with no ground within `metres` of `at`, from the planet's
    // centre: a pass over all those near, so for now and then.
    void keep_near(const Layers& layers, DVec3 at, double metres);
    // The corners of the tile columns taken or let go since last asked, in order, each once.
    [[nodiscard]] std::vector<Site> take_changed();

private:
    Volume* volume_;
    std::map<Site, Coarse> coarse_;
    std::vector<Site> changed_;
};

} // namespace hog::globe
