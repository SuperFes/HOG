#include "test_planet.hpp"

#include <hog/client/tile_meshes.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using hog::client::GroundChanges;
using hog::client::tile_key;
using hog::client::TileMeshes;
using hog::globe::TileKey;

// Rock a few voxels deep right across a tile.
hog::protocol::SentTile rock_at(TileKey key) {
    const hog::globe::VoxelTree tree =
        hog::globe::VoxelTree::build(hog::globe::Volume::tile_side, [](int, int, int z) {
            return z < 10 ? hog::globe::Cube{.solid = hog::globe::Material::Granite,
                                             .solid_amount = hog::globe::Voxel::full}
                          : hog::globe::Cube{};
        });
    return {.level = key.level,
            .face = key.face,
            .i = key.i,
            .j = key.j,
            .k = key.k,
            .bytes = tree.encode()};
}

// Meshes what's taken, and what's drawn and gone of it.
GroundChanges settle(TileMeshes& meshes) {
    GroundChanges changes = meshes.collect();
    meshes.finish();
    GroundChanges more = meshes.collect();
    changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
    changes.gone.insert(changes.gone.end(), more.gone.begin(), more.gone.end());
    return changes;
}

bool has(const std::vector<GroundChanges::Shown>& shown, TileKey key) {
    return std::ranges::any_of(shown, [&](const auto& one) { return one.key == tile_key(key); });
}

} // namespace

TEST_CASE("a tile is drawn once meshed, until the finer sent after it are drawn all through it",
          "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    const TileKey parent{.level = 1, .face = 2, .i = 3, .j = 3, .k = 0};
    REQUIRE(meshes.take(rock_at(parent)));
    GroundChanges changes = settle(meshes);
    CHECK(has(changes.shown, parent));
    CHECK_FALSE(changes.shown.front().ground->vertices.empty());
    CHECK(meshes.drawn(parent));
    CHECK(meshes.collect().empty());

    // Seven of its eight in: it's drawn still, and they wait.
    for (std::size_t octant = 0; octant < 7; ++octant) {
        REQUIRE(meshes.take(rock_at(parent.child(octant))));
    }
    changes = settle(meshes);
    CHECK(changes.shown.empty());
    CHECK(changes.gone.empty());
    CHECK(meshes.drawn(parent));

    // All eight: they're drawn in its place, and it's let go.
    REQUIRE(meshes.take(rock_at(parent.child(7))));
    changes = settle(meshes);
    CHECK(changes.gone == std::vector{tile_key(parent)});
    CHECK(changes.shown.size() == 8);
    CHECK_FALSE(meshes.holds(parent));
    for (std::size_t octant = 0; octant < 8; ++octant) {
        CHECK(meshes.drawn(parent.child(octant)));
    }

    // Sent again over them, it's drawn in place of all eight.
    REQUIRE(meshes.take(rock_at(parent)));
    changes = settle(meshes);
    CHECK(changes.shown.size() == 1);
    CHECK(has(changes.shown, parent));
    CHECK(changes.gone.size() == 8);
    for (std::size_t octant = 0; octant < 8; ++octant) {
        CHECK_FALSE(meshes.holds(parent.child(octant)));
    }
}

TEST_CASE("a tile is meshed again when a coarser tile beside it comes, reading it at its edge",
          "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    const auto all_of = [](TileKey key, hog::globe::Cube cube) {
        return hog::protocol::SentTile{
            .level = key.level,
            .face = key.face,
            .i = key.i,
            .j = key.j,
            .k = key.k,
            .bytes = hog::globe::VoxelTree{hog::globe::Volume::tile_side, cube}.encode()};
    };
    // Rock to its top, with nothing known over it: it goes on up, so nothing of it shows.
    const TileKey rock{.level = 0, .face = 2, .i = 6, .j = 6, .k = 3};
    REQUIRE(meshes.take(all_of(
        rock, {.solid = hog::globe::Material::Granite, .solid_amount = hog::globe::Voxel::full})));
    GroundChanges changes = settle(meshes);
    REQUIRE(has(changes.shown, rock));
    CHECK(changes.shown.front().ground->vertices.empty());

    // Air sent two levels coarser over it: its top shows.
    const TileKey air{.level = 2, .face = 2, .i = 1, .j = 1, .k = 1};
    REQUIRE(meshes.take(all_of(air, {})));
    changes = settle(meshes);
    const auto shown = std::ranges::find(changes.shown, tile_key(rock), &GroundChanges::Shown::key);
    REQUIRE(shown != changes.shown.end());
    CHECK_FALSE(shown->ground->vertices.empty());
}

TEST_CASE("tiles that aren't, or aren't on the planet, are refused", "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    hog::protocol::SentTile garbled = rock_at({.face = 1});
    garbled.bytes.resize(3);
    CHECK_FALSE(meshes.take(garbled).has_value());
    CHECK_FALSE(meshes.take(rock_at({.face = 6})).has_value());
    CHECK_FALSE(meshes.take(rock_at({.level = 40, .face = 1})).has_value());
    CHECK(meshes.collect().empty());
}
