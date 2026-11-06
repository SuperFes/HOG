#include <hog/server/ground_bake.hpp>

#include <hog/globe/cell.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>

using Catch::Approx;
using hog::DVec3;
using hog::server::GroundBake;
using hog::server::RainRecord;

namespace {

struct Earth {
    hog::globe::Generator generator{hog::globe::Planet{}};
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
};

// A point on land, well clear of the sea.
DVec3 on_land(const Earth& earth) {
    const DVec3 start = glm::normalize(DVec3{0.3, 0.4, 0.86});
    return hog::globe::land_near(earth.generator, start).value_or(start);
}

} // namespace

TEST_CASE("the tiles due round a place are its own first, then outward, never twice",
          "[server][bake]") {
    const Earth earth;
    const DVec3 up = on_land(earth);
    GroundBake bake;
    const std::array places{up, up};
    const std::vector<GroundBake::Tile> due = bake.due(earth.store, places, 1);
    REQUIRE_FALSE(due.empty());
    CHECK(due.front() == GroundBake::tile_at(earth.store, up));
    CHECK(due.size() == 9);
    bake.mark(due.front());
    const std::vector<GroundBake::Tile> after = bake.due(earth.store, places, 1);
    CHECK(after.size() == 8);
    CHECK(std::ranges::find(after, due.front()) == after.end());
}

TEST_CASE("the baked tiles are kept and read back", "[server][bake]") {
    GroundBake bake;
    bake.mark({.face = 2, .i = 7, .j = 300});
    bake.mark({.face = 5, .i = 0, .j = 1});
    GroundBake again;
    REQUIRE(again.restore(bake.saved()));
    CHECK(again.size() == 2);
    CHECK(again.baked({.face = 2, .i = 7, .j = 300}));
    CHECK_FALSE(again.baked({.face = 2, .i = 7, .j = 301}));
    std::vector<std::byte> cut = bake.saved();
    cut.pop_back();
    CHECK_FALSE(again.restore(cut));
}

TEST_CASE("a tile's bricks hold its ground", "[server][bake]") {
    const Earth earth;
    const DVec3 up = on_land(earth);
    const GroundBake::Tile tile = GroundBake::tile_at(earth.store, up);
    const std::vector<hog::globe::BrickSite> bricks = GroundBake::bricks_of(earth.world, tile);
    const hog::globe::Layers& layers = earth.generator.layers();
    const hog::globe::Cell cell = hog::globe::cell_under(up, layers.level);
    const int ground = layers.layer_at(hog::globe::top_of(earth.world, up).value_or(0.0));
    const hog::globe::BrickSite home = hog::globe::brick_of(
        {.face = cell.face, .i = cell.i, .j = cell.j, .layer = static_cast<std::uint32_t>(ground)});
    CHECK(std::ranges::find(bricks, home) != bricks.end());
    // A few bricks up each stack, not the whole of it.
    const std::size_t stacks = std::size_t{GroundBake::tile_side} * GroundBake::tile_side;
    CHECK(bricks.size() >= 3 * stacks);
    CHECK(bricks.size() < 12 * stacks);
}

TEST_CASE("the rain a place gets is the mean of what the weather let fall there",
          "[server][bake]") {
    RainRecord rain{3, 0.6};
    const DVec3 up = glm::normalize(DVec3{1.0, 0.2, 0.1});
    CHECK(rain.at(up) == 0.6);
    rain.add([](DVec3) { return std::optional<float>{2.0F}; }, 10.0);
    rain.add([](DVec3) { return std::optional<float>{0.0F}; }, 30.0);
    // Half a millimetre an hour on average, the year round.
    CHECK(rain.at(up) == Approx(0.5 * 365.25 * 24.0 / 1'000.0));
}

TEST_CASE("baking a tile under rain fills a hollow dug in it with water voxels",
          "[server][bake]") {
    Earth earth;
    const DVec3 up = on_land(earth);
    const hog::globe::Layers& layers = earth.generator.layers();
    const hog::globe::Cell cell = hog::globe::cell_under(up, layers.level);
    const DVec3 middle = hog::globe::center(cell);
    const double ground = hog::globe::top_of(earth.world, middle).value_or(0.0);
    hog::globe::dig(earth.world, middle * ground, 3.0);
    const GroundBake::Tile tile = GroundBake::tile_at(earth.store, middle);
    const RainRecord rain{3, 2.0};
    // On a copy of the tile's held bricks, giving back what it changed.
    const std::vector<GroundBake::Baked> written = GroundBake::bake(
        earth.generator, GroundBake::held_in(earth.store, tile), tile, rain, 0.5);
    REQUIRE_FALSE(written.empty());
    for (const GroundBake::Baked& baked : written) {
        if (baked.brick) {
            earth.store.put(baked.site, *baked.brick);
        } else {
            earth.store.erase(baked.site);
        }
    }
    // The pit's floor is under water.
    const int floor = layers.layer_at(
        hog::globe::surface_under(earth.world, middle, ground).value_or(0.0));
    const hog::globe::Voxel over = earth.world.at({.face = cell.face,
                                                   .i = cell.i,
                                                   .j = cell.j,
                                                   .layer = static_cast<std::uint32_t>(floor + 1)});
    CHECK(over.liquid == hog::globe::Material::Water);
    CHECK(over.liquid_amount > 0);
}
