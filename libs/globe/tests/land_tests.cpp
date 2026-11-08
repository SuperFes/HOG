#include "bed.hpp"

#include <hog/globe/land.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

using hog::globe::Cell;
using hog::globe::Cube;
using hog::globe::Land;
using hog::globe::Site;
using hog::globe::TileKey;
using hog::globe::VoxelTree;

namespace {

constexpr int side = hog::globe::Volume::tile_side;

bool same_voxels(const VoxelTree& a, const VoxelTree& b) {
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                if (!(a.at(x, y, z) == b.at(x, y, z))) {
                    return false;
                }
            }
        }
    }
    return true;
}

// The finest tile at the ground somewhere gentle.
TileKey ground_tile(const hog::globe::Generator& generator) {
    const hog::DVec3 up = hog::globe::bed::ground_between(generator, 10.0, 300.0);
    const Cell cell = hog::globe::cell_under(up, generator.layers().level);
    const hog::globe::Layers& layers = generator.layers();
    const int ground = layers.layer_at(hog::globe::ground_below(generator, up, 0.5).value_or(0.0));
    return {.face = cell.face,
            .i = cell.i / side,
            .j = cell.j / side,
            .k = static_cast<std::uint32_t>(ground / side)};
}

} // namespace

TEST_CASE("a tile asked of the land at any level is the generator's at that level",
          "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    CHECK_FALSE(land.known(fine));
    CHECK(land.at({.face = fine.face, .i = fine.i * side, .j = fine.j * side}) == Cube{});

    const TileKey coarse = fine.parent().parent();
    CHECK(same_voxels(land.tile(coarse), hog::globe::generate_tile(generator, coarse)));
    CHECK(land.known(coarse));
    CHECK_FALSE(land.known(fine));

    CHECK(land.tile(fine).same_as(hog::globe::generate_tile(generator, fine)));
    CHECK(land.known(fine));
}

TEST_CASE("the land's coarser tiles stand for what's known finer under them", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const VoxelTree made = land.tile(fine);
    const TileKey parent = fine.parent();
    const VoxelTree coarse = land.tile(parent);
    const VoxelTree generated = hog::globe::generate_tile(generator, parent);
    const int half = side / 2;
    const int at_x = static_cast<int>(fine.i % 2) * half;
    const int at_y = static_cast<int>(fine.j % 2) * half;
    const int at_z = static_cast<int>(fine.k % 2) * half;
    bool fine_eighth = true;
    bool generated_rest = true;
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const bool in = x >= at_x && x < at_x + half && y >= at_y && y < at_y + half &&
                                z >= at_z && z < at_z + half;
                if (in) {
                    fine_eighth = fine_eighth &&
                                  coarse.at(x, y, z) == made.average(2 * (x - at_x), 2 * (y - at_y),
                                                                     2 * (z - at_z), 2);
                } else {
                    generated_rest = generated_rest && coarse.at(x, y, z) == generated.at(x, y, z);
                }
            }
        }
    }
    CHECK(fine_eighth);
    CHECK(generated_rest);
}

TEST_CASE("a voxel set in the land is kept, and stood for at every level over it",
          "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const Site site{.face = fine.face,
                    .i = (fine.i * side) + 9,
                    .j = (fine.j * side) + 20,
                    .layer = (fine.k * side) + 33};
    const Cube water{.liquid = hog::globe::Material::Water,
                     .liquid_amount = hog::globe::Voxel::full};
    land.set(site, water);
    CHECK(land.at(site) == water);
    CHECK(land.known(fine));

    // Kept when coarser ground about it is made after.
    const TileKey coarse = fine.parent().parent();
    const VoxelTree over = land.tile(coarse);
    CHECK(land.at(site) == water);
    const VoxelTree tile = land.tile(fine);
    CHECK(tile.at(9, 20, 33) == water);
    const int x = static_cast<int>((fine.i % 4) * side) + 9;
    const int y = static_cast<int>((fine.j % 4) * side) + 20;
    const int z = static_cast<int>((fine.k % 4) * side) + 33;
    CHECK(over.at(x / 4, y / 4, z / 4) == tile.average(8, 20, 32, 4));
}

TEST_CASE("the land makes only what it doesn't know, spread as asked", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const std::array keys{fine, fine.parent(), fine, fine.parent().parent()};
    std::size_t spread = 0;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        spread += count;
        for (std::size_t k = 0; k < count; ++k) {
            body(k);
        }
    };
    land.make(keys, each);
    CHECK(spread == 3);
    land.make(keys, each);
    CHECK(spread == 3);
    for (const TileKey key : keys) {
        CHECK(land.known(key));
    }
}
