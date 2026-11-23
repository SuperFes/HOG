#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/asset.hpp>
#include <hog/flora/woods.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog::flora {

// Where single trees stand and who they are, from hashes alone, so every machine places the same
// trees. Only arithmetic and square roots decide anything here (docs/woods.md).

// Two unit vectors across the ground a level `up` faces, the same on every machine.
struct Across {
    DVec3 east{1.0, 0.0, 0.0};
    DVec3 north{0.0, 1.0, 0.0};
};
[[nodiscard]] Across across_of(DVec3 up) noexcept;

// A place a tree may stand, on the level through a node's middle, and its own hash.
struct Spot {
    DVec3 at{0.0};
    std::uint64_t hash = 0;
};

// One spot in each cell `spacing` across, jittered within it, over a square on the level through
// `middle` covering all a node `side` across shows when looked at down `up`. Whoever stands them
// on the ground keeps those whose ground lies within the node.
[[nodiscard]] std::vector<Spot> spots(DVec3 middle, double side, DVec3 up, std::uint64_t node_hash,
                                      double spacing);

// The species a placed tree may be, each with the kind it belongs to and how many variants it
// carries, the first `impostors` of them with an impostor.
struct Planting {
    struct Species {
        Kind kind = Kind::Deciduous;
        std::uint32_t variants = 1;
        std::uint32_t impostors = 1;
    };
    std::vector<Species> species;
    double spacing = 7.0; // metres between spots
};

// The kind a species the tree lab grows belongs to, by its name; none for one it doesn't know.
[[nodiscard]] std::optional<Kind> kind_of(std::string_view species) noexcept;

// Planting from species as loaded, in their order, with no more than `most_variants` of each;
// a species of no known kind, or without variants, is never planted.
[[nodiscard]] Planting planting_of(std::span<const Species> species, std::uint32_t most_variants);

// Who stands at a spot, if anyone: chosen by the cover, then a kind by its share, then a species of
// that kind (or of the nearest kind that has any), a variant, and the tree's own turn, size, lean,
// tint and season.
struct Planted {
    std::uint32_t species = 0;
    std::uint32_t variant = 0;
    float turn = 0.0F;        // about up, 0 to 1 of a full turn
    float scale = 1.0F;       // of the variant's size
    Vec2 lean{0.0F};          // along east and north, as a slope
    float tint = 0.0F;        // -1 to 1, its leaves' colour off its species'
    std::uint32_t season = 0; // its hash for own_season
};
[[nodiscard]] std::optional<Planted> plant(const Spot& spot, const Woods& woods,
                                           const Planting& planting) noexcept;

// The hash's top 53 bits as 0 to 1.
[[nodiscard]] constexpr double unit_of(std::uint64_t hash) noexcept {
    return static_cast<double>(hash >> 11U) * 0x1.0p-53;
}

} // namespace hog::flora
