#include <hog/globe/held_tiles.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

using hog::globe::Cube;
using hog::globe::HeldTiles;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::TileKey;
using hog::globe::Volume;
using hog::globe::Voxel;
using hog::globe::VoxelTree;

namespace {

constexpr std::uint32_t side = Volume::tile_side;

const Cube rock{.solid = Material::Granite, .solid_amount = Voxel::full};

// Rock up to `height` voxels of the tile, air over it.
HeldTiles::Tree ground_to(int height) {
    return std::make_shared<const VoxelTree>(VoxelTree::build(
        Volume::tile_side, [&](int, int, int z) { return z < height ? rock : Cube{}; }));
}

HeldTiles::Tree air() {
    return std::make_shared<const VoxelTree>(Volume::tile_side);
}

} // namespace

TEST_CASE("a voxel is as the finest tile held over it has it", "[globe][held_tiles]") {
    HeldTiles held;
    const TileKey coarse{.level = 2, .face = 3, .i = 1, .j = 1, .k = 0};
    held.put(coarse, ground_to(10));
    // Each of its voxels stands for four a side: rock to layer 40.
    const Site corner{.face = 3, .i = 4 * side, .j = 4 * side, .layer = 0};
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 39}) == rock);
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 40}) == Cube{});
    CHECK(held.finest_over({.level = 0, .face = 3, .i = 5, .j = 6, .k = 3})->key == coarse);
    CHECK_FALSE(held.finest_over({.level = 0, .face = 3, .i = 8, .j = 4, .k = 0}));
    CHECK(held.at({.face = 3, .i = 8 * side, .j = 4 * side, .layer = 0}) == Cube{});

    // Finer where sent finer, coarser beside it.
    const TileKey fine{.level = 0, .face = 3, .i = 4, .j = 4, .k = 0};
    held.put(fine, ground_to(20));
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 19}) == rock);
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 20}) == Cube{});
    CHECK(held.at({.face = 3, .i = corner.i + side, .j = corner.j, .layer = 39}) == rock);
    CHECK(held.size() == 2);

    // Sent coarser over it again, the finer is let go; those beside stay.
    const TileKey other{.level = 0, .face = 3, .i = 8, .j = 4, .k = 0};
    held.put(other, ground_to(5));
    held.put(coarse, ground_to(1));
    CHECK(held.size() == 2);
    CHECK(held.tile(fine) == nullptr);
    CHECK(held.tile(other) != nullptr);
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 3}) == rock);
    CHECK(held.at({.face = 3, .i = corner.i, .j = corner.j, .layer = 4}) == Cube{});
}

TEST_CASE("the top of a column held is found down through its tiles, any level",
          "[globe][held_tiles]") {
    HeldTiles held;
    // Open air sent coarse over finer ground, and the deep ground coarse under it.
    held.put({.level = 2, .face = 0, .i = 0, .j = 0, .k = 1}, air());
    held.put({.level = 0, .face = 0, .i = 0, .j = 0, .k = 3}, ground_to(7));
    held.put({.level = 0, .face = 0, .i = 0, .j = 0, .k = 2}, ground_to(64));
    held.put({.level = 0, .face = 0, .i = 1, .j = 0, .k = 3}, air());
    held.put({.level = 0, .face = 0, .i = 1, .j = 0, .k = 2}, ground_to(64));
    held.put({.level = 1, .face = 0, .i = 0, .j = 0, .k = 0}, ground_to(64));
    const std::uint32_t over = 8 * side;
    CHECK(held.top({.face = 0, .i = 3, .j = 3}, over) == (3 * side) + 6);
    // Under a tile of air, the ground below it, to its top.
    CHECK(held.top({.face = 0, .i = side + 3, .j = 3}, over) == (3 * side) - 1);
    // A coarse voxel holding matter tops out at its own top, never past where it's asked under.
    held.put({.level = 0, .face = 0, .i = 1, .j = 0, .k = 2}, air());
    held.put({.level = 1, .face = 0, .i = 0, .j = 0, .k = 0}, ground_to(5));
    CHECK(held.top({.face = 0, .i = side + 3, .j = 3}, over) == 9);
    CHECK(held.top({.face = 0, .i = side + 3, .j = 3}, 9) == 8);
    CHECK_FALSE(held.top({.face = 0, .i = 5 * side, .j = 3}, over));
}
