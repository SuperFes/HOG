#include <hog/core/assert.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/material.hpp>

#include <algorithm>
#include <iterator>

namespace hog::globe {
namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

TileKey finest_holding(Site site) noexcept {
    return {.level = 0,
            .face = site.face,
            .i = site.i / side,
            .j = site.j / side,
            .k = site.layer / side};
}

bool uniform_air(const VoxelTree& tree) {
    return tree.leaf_side(0, 0, 0) == Volume::tile_side && tree.at(0, 0, 0) == Cube{};
}

bool holds_matter(const Cube& cube) noexcept {
    return cube.solid != Material::Air || cube.grain != Material::Air ||
           cube.liquid != Material::Air;
}

} // namespace

void HeldTiles::put(TileKey key, Tree tree) {
    HOG_ASSERT(tree != nullptr && tree->side() == Volume::tile_side, "a tile");
    for (std::uint8_t level = 0; level < key.level; ++level) {
        const auto finer = static_cast<unsigned>(key.level - level);
        const std::uint32_t across = 1U << finer;
        for (std::uint32_t i = key.i << finer; i < (key.i << finer) + across; ++i) {
            const std::uint32_t j_from = key.j << finer;
            auto at =
                tiles_.lower_bound({.level = level, .face = key.face, .i = i, .j = j_from, .k = 0});
            while (at != tiles_.end() && at->first.level == level && at->first.face == key.face &&
                   at->first.i == i && at->first.j < j_from + across) {
                const std::uint32_t k = at->first.k;
                at = k >= (key.k << finer) && k < (key.k << finer) + across ? tiles_.erase(at)
                                                                            : std::next(at);
            }
        }
    }
    coarsest_ = std::max(coarsest_, key.level);
    tiles_.insert_or_assign(key, std::move(tree));
}

const VoxelTree* HeldTiles::tile(TileKey key) const {
    const auto found = tiles_.find(key);
    return found == tiles_.end() ? nullptr : found->second.get();
}

std::optional<HeldTile> HeldTiles::finest_over(TileKey key) const {
    for (; key.level <= coarsest_; key = key.parent()) {
        if (const VoxelTree* found = tile(key)) {
            return HeldTile{.key = key, .tree = found};
        }
    }
    return std::nullopt;
}

Cube HeldTiles::at(Site site) const {
    const std::optional<HeldTile> held = finest_over(finest_holding(site));
    if (!held) {
        return {};
    }
    const unsigned level = held->key.level;
    const auto place = [&](std::uint32_t at, std::uint32_t tile) {
        return static_cast<int>((at >> level) - (tile * side));
    };
    return held->tree->at(place(site.i, held->key.i), place(site.j, held->key.j),
                          place(site.layer, held->key.k));
}

std::optional<std::uint32_t> HeldTiles::top(Site site, std::uint32_t below) const {
    std::int64_t layer = std::int64_t{below} - 1;
    while (layer >= 0) {
        site.layer = static_cast<std::uint32_t>(layer);
        const std::optional<HeldTile> held = finest_over(finest_holding(site));
        if (!held) {
            layer = (layer / side * side) - 1;
            continue;
        }
        const unsigned level = held->key.level;
        const std::int64_t lowest = std::int64_t{held->key.k} * side << level;
        if (!uniform_air(*held->tree)) {
            const auto x = static_cast<int>((site.i >> level) - (held->key.i * side));
            const auto y = static_cast<int>((site.j >> level) - (held->key.j * side));
            for (auto z = static_cast<int>((layer >> level) - (lowest >> level)); z >= 0; --z) {
                if (holds_matter(held->tree->at(x, y, z))) {
                    const std::int64_t over = ((lowest >> level) + z + 1) << level;
                    return static_cast<std::uint32_t>(std::min(over, std::int64_t{below}) - 1);
                }
            }
        }
        layer = lowest - 1;
    }
    return std::nullopt;
}

} // namespace hog::globe
