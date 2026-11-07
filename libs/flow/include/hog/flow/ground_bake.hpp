#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::flow {

class RainRecord;

// Ground the long work of the weather has been run over once, the first time anyone came near:
// tiles of brick stacks whose water has been let settle in their voxels. What's baked is kept with
// the world, so it's done once for everyone.
class GroundBake {
public:
    // Brick stacks along each side of a tile.
    static constexpr std::uint32_t tile_side = 16;

    struct Tile {
        std::uint8_t face = 0;
        std::uint32_t i = 0; // in tiles across the face
        std::uint32_t j = 0;
        friend constexpr auto operator<=>(const Tile&, const Tile&) noexcept = default;
    };

    // The tile the point in the direction `up` (unit, from the centre) lies in.
    [[nodiscard]] static Tile tile_at(const globe::VoxelStore& store, DVec3 up);
    // The tiles within `reach` tiles of each place's own, along and across its face, that aren't
    // baked: each place's own first, then outward.
    [[nodiscard]] std::vector<Tile> due(const globe::VoxelStore& store, std::span<const DVec3> places,
                                        int reach) const;
    // The bricks a tile's top ground lies in: in each of its stacks, from two bricks under its
    // highest surface to a brick over the highest layer that may hold matter.
    [[nodiscard]] static std::vector<globe::BrickSite> bricks_of(const globe::World& world,
                                                                 Tile tile);

    // A drainage's river through the cells of the generator's drainage, as it crosses the tiles
    // in one: the river each tile on its course takes in, over the edge it comes in by.
    using Course = std::map<Tile, Region::River>;
    // The course of the river through the drainage cell the tile lies in: from its edge towards
    // the cell upstream that gives it most, to its edge towards the cell it drains into, along
    // the lowest of the tiles' ground; none where too little drains through to be a river.
    [[nodiscard]] static Course course(const globe::World& world, Tile tile,
                                       const RainRecord& rain);
    // Springs seeded in the tile: now and then one, at a column picked by the tile.
    [[nodiscard]] static std::vector<Region::Spring> springs(Tile tile);

    // A brick a bake wrote, or none where it came out as generated.
    struct Baked {
        globe::BrickSite site;
        std::optional<globe::Brick> brick;
    };
    // The bricks held in the tile's stacks, copied for a bake to start from.
    [[nodiscard]] static std::vector<Baked> held_in(const globe::VoxelStore& store, Tile tile);
    // Settles the rain on the tile's ground, starting from `held` over what's generated, and
    // gives back the bricks it changed. Reads and writes nothing else, so it can run anywhere.
    // Open water gives back `evaporation` metres a year.
    [[nodiscard]] static std::vector<Baked> bake(const globe::Generator& generator,
                                                 const std::vector<Baked>& held, Tile tile,
                                                 const RainRecord& rain, double evaporation);

    void mark(Tile tile) { baked_.insert(tile); }
    [[nodiscard]] bool baked(Tile tile) const { return baked_.contains(tile); }
    [[nodiscard]] std::size_t size() const noexcept { return baked_.size(); }

    [[nodiscard]] std::vector<std::byte> saved() const;
    [[nodiscard]] Result<void> restore(std::span<const std::byte> bytes);

private:
    std::set<Tile> baked_;
};

// The rain each place gets in a year, as the weather has given it: the mean over the weather's
// cells of what falls, kept as weather is made.
class RainRecord {
public:
    // Over the cells of `level`; until any is known, a place gets `otherwise` metres a year.
    explicit RainRecord(int level, double otherwise = 0.8);

    // Takes in `hours` of the rain falling now, in mm an hour, by the way up a place stands;
    // none where the weather doesn't say.
    template <typename Falling>
    void add(const Falling& falling, double hours);
    // Metres a year.
    [[nodiscard]] double at(DVec3 up) const;

private:
    void add_cell(std::size_t cell, double mm_an_hour, double hours);
    [[nodiscard]] DVec3 middle_of(std::size_t cell) const;

    int level_;
    double otherwise_;
    std::vector<double> sum_;   // mm fallen
    std::vector<double> hours_; // hours told of
};

template <typename Falling>
void RainRecord::add(const Falling& falling, double hours) {
    for (std::size_t cell = 0; cell < sum_.size(); ++cell) {
        if (const auto mm = falling(middle_of(cell))) {
            add_cell(cell, static_cast<double>(*mm), hours);
        }
    }
}

} // namespace hog::flow
