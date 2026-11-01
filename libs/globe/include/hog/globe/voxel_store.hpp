#pragma once

#include <hog/globe/brick.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hog::globe {

// What has changed of a world, held a brick at a time; wherever nothing is held, the generator
// says what's there. A brick changed anywhere is held whole, as it now is.
class VoxelStore {
public:
    // Precondition: a face is at least a brick across.
    explicit VoxelStore(const Layers& layers);

    [[nodiscard]] const Layers& layers() const noexcept { return layers_; }

    // Bricks across a face, and up the layers; the top one may run above the ceiling.
    [[nodiscard]] std::uint32_t bricks_across() const noexcept;
    [[nodiscard]] std::uint32_t bricks_up() const noexcept;
    [[nodiscard]] bool holds_site(BrickSite brick) const noexcept;

    // None where the voxel is as generated.
    [[nodiscard]] std::optional<Voxel> at(Site site) const;
    // Null where the brick is as generated.
    [[nodiscard]] const Brick* brick(BrickSite site) const;

    // Holds `brick` in place of what was generated or held before.
    void put(BrickSite site, Brick brick);
    // Lets the brick go, so the generator says again.
    void erase(BrickSite site);

    [[nodiscard]] std::size_t size() const noexcept { return bricks_.size(); }
    // Held, by the bricks and the table of them.
    [[nodiscard]] std::size_t bytes() const noexcept;
    // Every brick held, in order.
    [[nodiscard]] std::vector<BrickSite> sites() const;

    // The bricks put or let go since this was last called, in order.
    [[nodiscard]] std::vector<BrickSite> take_changed();

private:
    [[nodiscard]] static std::uint64_t key_of(BrickSite site) noexcept;
    [[nodiscard]] static BrickSite site_of(std::uint64_t key) noexcept;

    Layers layers_;
    std::unordered_map<std::uint64_t, Brick> bricks_;
    std::unordered_set<std::uint64_t> changed_;
};

} // namespace hog::globe
