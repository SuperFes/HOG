#include "bed.hpp"

#include <hog/globe/column_cache.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

using hog::globe::ColumnCache;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::globe::Voxel;
using hog::globe::bed::earth;

// A tile column of granite up to `top` over a floor at 0.
hog::globe::TileColumn rock_to(std::uint32_t top) {
    Volume volume;
    volume.set_floor({}, 0);
    for (std::uint32_t layer = 0; layer <= top; ++layer) {
        volume.set({.face = 0, .i = 3, .j = 4, .layer = layer},
                   {.solid = Material::Granite, .solid_amount = Voxel::full});
    }
    return volume.column({}).value_or(hog::globe::TileColumn{});
}

} // namespace

TEST_CASE("a tile column sent at its own level is held in the volume, coarser beside it",
          "[globe][column_cache]") {
    Volume volume;
    ColumnCache cache{volume};
    const hog::globe::TileColumn column = rock_to(70);
    const Site corner{.face = 2, .i = 5 * 64, .j = 9 * 64};

    REQUIRE(cache.take(2, 5, 9, 0, column.encode()));
    CHECK(volume.at({.face = 2, .i = (5 * 64) + 3, .j = (9 * 64) + 4, .layer = 70}).solid ==
          Material::Granite);
    CHECK(cache.coarse(corner) == nullptr);
    CHECK(cache.take_changed() == std::vector<Site>{corner});
    CHECK(cache.take_changed().empty());

    // Coarser, it's no longer in the volume.
    REQUIRE(cache.take(2, 5, 9, 2, column.halved().halved().encode()));
    CHECK_FALSE(volume.floor(corner).has_value());
    const ColumnCache::Coarse* coarse = cache.coarse(corner);
    REQUIRE(coarse != nullptr);
    CHECK(coarse->level == 2);
    CHECK(coarse->column.tiles.front().second.side() == 16);
    CHECK(coarse->column.tiles.back().second.at(0, 1, (70 / 4) - 16).solid == Material::Granite);

    // Bytes not of their level change nothing.
    CHECK_FALSE(cache.take(2, 5, 9, 1, column.encode()));
    CHECK(cache.coarse(corner)->level == 2);
    CHECK_FALSE(cache.take(2, 5, 9, 9, column.encode()));

    // Nothing made there.
    REQUIRE(cache.take(2, 5, 9, 2, {}));
    CHECK(cache.coarse(corner) == nullptr);
}

TEST_CASE("a client lets go the tile columns far from it", "[globe][column_cache]") {
    const hog::globe::Layers& layers = earth().layers();
    const hog::DVec3 at = glm::normalize(hog::DVec3{0.3, 0.4, 0.86}) * earth().planet().radius;
    const std::vector<Site> near = hog::globe::tile_columns_near(layers, at, 300.0);
    const std::vector<Site> further = hog::globe::tile_columns_near(layers, at, 2000.0);
    REQUIRE(further.size() > near.size());
    Volume volume;
    ColumnCache cache{volume};
    const hog::globe::TileColumn column = rock_to(10);
    for (std::size_t k = 0; k < further.size(); ++k) {
        const Site corner = further[k];
        const auto level = static_cast<std::uint8_t>(k % 2);
        REQUIRE(cache.take(corner.face, corner.i / 64, corner.j / 64, level,
                           level == 0 ? column.encode() : column.halved().encode()));
    }
    (void)cache.take_changed();
    cache.keep_near(layers, at, 300.0);
    const std::vector<Site> dropped = cache.take_changed();
    CHECK(dropped.size() == further.size() - near.size());
    for (const Site corner : further) {
        const bool kept = std::ranges::binary_search(near, corner);
        CHECK((volume.floor(corner).has_value() || cache.coarse(corner) != nullptr) == kept);
    }
}

TEST_CASE("a world reads the tile columns held coarser where it holds none of its own",
          "[globe][column_cache]") {
    // Granite up to layer 40 right across a tile.
    const hog::globe::TileColumn column{
        .floor = 0,
        .tiles = {{0, hog::globe::VoxelTree::build(Volume::tile_side, [](int, int, int z) {
                       return z < 40 ? hog::globe::Cube{.solid = Material::Granite,
                                                        .solid_amount = Voxel::full}
                                     : hog::globe::Cube{};
                   })}}};
    Volume volume;
    hog::globe::World world{earth(), volume};
    ColumnCache cache{volume};
    REQUIRE(cache.take(1, 2, 3, 2, column.halved().halved().encode()));
    const auto at = [&](std::uint32_t layer) {
        return world.at({.face = 1, .i = (2 * 64) + 17, .j = (3 * 64) + 50, .layer = layer});
    };
    CHECK(at(20).solid == Material::Air);

    world.read_coarse(&cache);
    CHECK(at(20).solid == Material::Granite);
    CHECK(at(39).solid == Material::Granite);
    CHECK(at(40).solid == Material::Air);
    const std::vector<hog::globe::Cell> cells{
        hog::globe::column_of({.face = 1, .i = 2 * 64, .j = 3 * 64}, earth().layers())};
    const std::vector<Voxel> filled = world.fill(cells, 0, 64);
    CHECK(filled[39].solid == Material::Granite);
    CHECK(filled[40].solid == Material::Air);

    // Its own level, once held, is read in place of the coarser.
    REQUIRE(cache.take(1, 2, 3, 0, hog::globe::TileColumn{.floor = 0, .tiles = {}}.encode()));
    CHECK(at(20).solid == Material::Air);
}
