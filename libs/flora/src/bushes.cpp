#include <hog/flora/bushes.hpp>
#include <hog/flora/stand.hpp>

#include <cstdint>
#include <optional>

namespace hog::flora {

namespace {

// a + t(b - a), spelled out: the ordinary rounding of each step is part of the contract.
float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

float saturated(float value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

float smoothstepped(float from, float to, float value) noexcept {
    const float t = saturated((value - from) / (to - from));
    return t * t * (3.0F - (2.0F * t));
}

// A bush's lattice cell across, in the trees' cells.
constexpr float trees_per_bush =
    1.0F / static_cast<float>(1U << (bush_lattice_level - lattice_level));

// How a bush's share of its cells is bent by the woods about it: crowding their sunny edges,
// thinning under the canopy, unless the trees there are gone.
float under_woods(const BiomeTrees& trees, Vec2 cell, std::uint32_t face, float gone) noexcept {
    const float field = stand_field(cell * trees_per_bush, face);
    const float canopy = smoothstepped(trees.edge, trees.edge + 0.1F, field) * (1.0F - gone);
    const float off_hem = (field - trees.edge - 0.05F) / 0.1F;
    const float hem = saturated(1.0F - (off_hem < 0.0F ? -off_hem : off_hem));
    return mixed(1.0F, 0.4F, canopy) + (0.8F * hem);
}

} // namespace

Vec2 bush_spot(std::uint32_t lattice_i, std::uint32_t lattice_j, std::uint32_t face) noexcept {
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    return {unit(hash), unit(hash + 1U)};
}

Vec2 bush_stray(std::uint32_t lattice_i, std::uint32_t lattice_j, std::uint32_t face) noexcept {
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    return {(2.0F * unit(hash + 8U)) - 1.0F, (2.0F * unit(hash + 9U)) - 1.0F};
}

std::optional<Shrub> bush_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                std::uint32_t face, planet::Biome biome,
                                const Fate& fate) noexcept {
    const BiomeBushes bushes = bushes_of(biome);
    if (bushes.height <= 0.0F) {
        return std::nullopt;
    }
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    const Vec2 spot{unit(hash), unit(hash + 1U)};
    float share = bushes.share;
    float fallen = 0.0F;
    const BiomeTrees trees = trees_of(biome);
    if (trees.inside > 0.0F) {
        const float gone = saturated(1.0F - fate.living);
        fallen = saturated(gone - fate.dead);
        const Vec2 cell{static_cast<float>(lattice_i) + spot.x,
                        static_cast<float>(lattice_j) + spot.y};
        share *= under_woods(trees, cell, face, gone);
    }
    share = (share + (shrubs_in_fallen * fallen)) * (1.0F - saturated(fate.scar));
    if (unit(hash + 2U) >= share) {
        return std::nullopt;
    }

    const float roll = unit(hash + 3U);
    BushKind kind = BushKind::Mat;
    if (roll < bushes.floofy) {
        kind = BushKind::Floofy;
    } else if (roll < bushes.floofy + bushes.spiky) {
        kind = BushKind::Spiky;
    } else if (roll < bushes.floofy + bushes.spiky + bushes.twiggy) {
        kind = BushKind::Twiggy;
    }
    float height = bushes.height * (0.7F + (0.6F * unit(hash + 4U)));
    float width = height * 1.4F;
    switch (kind) {
    case BushKind::Spiky:
        width = height;
        break;
    case BushKind::Twiggy:
        width = height * 1.2F;
        break;
    case BushKind::Mat:
        height *= 0.4F;
        width = height * 2.8F;
        break;
    case BushKind::Floofy:
        break;
    }
    return Shrub{.spot = spot, .height = height, .width = width, .hash = hash, .kind = kind};
}

} // namespace hog::flora
