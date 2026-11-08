#include <hog/globe/land.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>

#include <algorithm>
#include <vector>

namespace hog::globe {

namespace {

// The mark of ground nothing is known of yet, coarser than any level.
constexpr std::uint8_t unknown = 255;

} // namespace

Land::Land(const Generator& generator)
    : generator_{&generator}, top_{top_level(generator.layers())} {}

Land::Place Land::place_of(TileKey key) const {
    const int shift = top_ - key.level;
    const int across = Volume::tile_side << key.level;
    const TileKey root{.level = top_,
                       .face = key.face,
                       .i = key.i >> shift,
                       .j = key.j >> shift,
                       .k = key.k >> shift};
    return {.root = root,
            .x = static_cast<int>(key.i - (root.i << shift)) * across,
            .y = static_cast<int>(key.j - (root.j << shift)) * across,
            .z = static_cast<int>(key.k - (root.k << shift)) * across,
            .across = across,
            .voxel = 1 << key.level};
}

const VoxelTree* Land::root(TileKey key) const {
    const auto found = roots_.find(key);
    return found == roots_.end() ? nullptr : &found->second;
}

VoxelTree& Land::root_for(TileKey key) {
    return roots_.try_emplace(key, Volume::tile_side << top_, Cube{}, unknown).first->second;
}

bool Land::known(TileKey key) const {
    const Place place = place_of(key);
    const VoxelTree* tree = root(place.root);
    return tree != nullptr &&
           tree->coarsest(place.x, place.y, place.z, place.across) <= made_at(key.level);
}

void Land::put(TileKey key, const VoxelTree& tile) {
    const Place place = place_of(key);
    root_for(place.root).graft(place.x, place.y, place.z, tile, place.voxel, made_at(key.level));
}

void Land::make(std::span<const TileKey> keys, const EachOf& each) {
    std::vector<TileKey> wanted;
    for (const TileKey key : keys) {
        if (!known(key)) {
            wanted.push_back(key);
        }
    }
    std::ranges::sort(wanted);
    const auto [last, end] = std::ranges::unique(wanted);
    wanted.erase(last, end);
    std::vector<VoxelTree> made(wanted.size(), VoxelTree{Volume::tile_side});
    const auto body = [&](std::size_t k) { made[k] = generate_tile(*generator_, wanted[k]); };
    if (each) {
        each(wanted.size(), body);
    } else {
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            body(k);
        }
    }
    for (std::size_t k = 0; k < wanted.size(); ++k) {
        put(wanted[k], made[k]);
    }
}

VoxelTree Land::tile(TileKey key) {
    if (!known(key)) {
        make(std::span{&key, 1});
    }
    const Place place = place_of(key);
    return root_for(place.root)
        .window(place.x, place.y, place.z, place.across, place.voxel, made_at(key.level));
}

namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

TileKey finest_key(Site site) noexcept {
    return {.face = site.face, .i = site.i / side, .j = site.j / side, .k = site.layer / side};
}

} // namespace

Cube Land::at(Site site) const {
    const Place place = place_of(finest_key(site));
    const VoxelTree* tree = root(place.root);
    if (tree == nullptr) {
        return {};
    }
    return tree->at(place.x + static_cast<int>(site.i % side),
                    place.y + static_cast<int>(site.j % side),
                    place.z + static_cast<int>(site.layer % side));
}

void Land::set(Site site, const Cube& voxel) {
    const TileKey key = finest_key(site);
    if (!known(key)) {
        make(std::span{&key, 1});
    }
    const Place place = place_of(key);
    root_for(place.root)
        .set(place.x + static_cast<int>(site.i % side), place.y + static_cast<int>(site.j % side),
             place.z + static_cast<int>(site.layer % side), voxel);
}

} // namespace hog::globe
