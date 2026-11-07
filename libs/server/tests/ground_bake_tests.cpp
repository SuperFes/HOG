#include <hog/server/ground_bake.hpp>

#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>
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

TEST_CASE("baking a tile under rain fills a hollow dug in it with water voxels or silt",
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
    const int floor = layers.layer_at(
        hog::globe::surface_under(earth.world, middle, ground).value_or(0.0));
    // Under water, or silted up by what the rain washed in, in the floor's voxel or over it.
    bool filled = false;
    for (int above = 0; above < 2; ++above) {
        const hog::globe::Voxel over =
            earth.world.at({.face = cell.face,
                            .i = cell.i,
                            .j = cell.j,
                            .layer = static_cast<std::uint32_t>(floor + above)});
        filled = filled || (over.liquid == hog::globe::Material::Water && over.liquid_amount > 0) ||
                 over.grain_amount > 0;
    }
    CHECK(filled);
}

TEST_CASE("baking a tile weathers the rock under its ground into loose layers",
          "[server][bake]") {
    Earth earth;
    const DVec3 up = on_land(earth);
    const GroundBake::Tile tile = GroundBake::tile_at(earth.store, up);
    const std::vector<GroundBake::Baked> written = GroundBake::bake(
        earth.generator, GroundBake::held_in(earth.store, tile), tile, RainRecord{3, 0.8}, 0.5);
    for (const GroundBake::Baked& baked : written) {
        if (baked.brick) {
            earth.store.put(baked.site, *baked.brick);
        }
    }
    const hog::globe::Layers& layers = earth.generator.layers();
    const hog::globe::Cell cell = hog::globe::cell_under(up, layers.level);
    const double ground = hog::globe::top_of(earth.world, up).value_or(0.0);
    const auto at = [&](std::uint32_t layer) {
        return earth.world.at({.face = cell.face, .i = cell.i, .j = cell.j, .layer = layer}).solid;
    };
    // The first shaped voxel down is loose, and some way under it there's rock again.
    auto layer = static_cast<std::uint32_t>(layers.layer_at(ground) + 1);
    while (at(layer) == hog::globe::Material::Air) {
        --layer;
    }
    CHECK(hog::globe::properties(at(layer)).repose > 0.0);
    CHECK(hog::globe::properties(at(layer - 12)).repose == 0.0);
}

namespace {

// A small planet worn by running water, made in seconds, so its drainage gathers rivers.
struct Worn {
    hog::globe::Generator generator{[] {
        auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
        planet.radius = 4'000.0;
        planet.floor_depth = 100.0;
        planet.sky_height = 500.0;
        planet.wear = 0.03;
        return planet;
    }()};
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
};

// A tile on land where a drainage cell gathers a river from the cell upstream of it.
template <typename Planet>
std::optional<GroundBake::Tile> tile_on_river(const Planet& earth, const RainRecord& rain) {
    const hog::globe::Generator::Shape& shape = earth.generator.shape();
    const auto sea = earth.generator.sea_level().value_or(0.0);
    for (std::uint64_t k = 0; k < shape.catchment.size(); k += 7) {
        const DVec3 up = hog::globe::center(hog::globe::cell_at_index(shape.cut_level, k));
        if (hog::globe::top_of(earth.world, up).value_or(0.0) < sea + 5.0) {
            continue;
        }
        const GroundBake::Tile tile = GroundBake::tile_at(earth.store, up);
        if (!GroundBake::course(earth.world, tile, rain).empty()) {
            return tile;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a river's course runs tile by tile across its drainage cell", "[server][bake]") {
    const Worn earth;
    const RainRecord rain{3, 1.0};
    const auto tile = tile_on_river(earth, rain);
    REQUIRE(tile);
    const GroundBake::Course course =
        GroundBake::course(earth.world, tile.value_or(GroundBake::Tile{}), rain);
    // Every tile's river comes in from a tile on the course beside it, or over the cell's edge.
    for (const auto& [on, river] : course) {
        CHECK(river.discharge > 0.0);
        constexpr std::array<std::pair<int, int>, 4> sides{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
        const auto [di, dj] = sides.at(river.side);
        const GroundBake::Tile from{.face = on.face,
                                    .i = static_cast<std::uint32_t>(static_cast<int>(on.i) + di),
                                    .j = static_cast<std::uint32_t>(static_cast<int>(on.j) + dj)};
        const bool joined = course.contains(from);
        const bool first = std::ranges::count_if(course, [&](const auto& other) {
                               return other.first == from;
                           }) == 0;
        CHECK((joined || first));
    }
}

TEST_CASE("baking a tile on a river's course lays a river in it", "[server][bake]") {
    const Worn earth;
    const RainRecord rain{3, 1.0};
    const auto tile = tile_on_river(earth, rain);
    REQUIRE(tile);
    const GroundBake::Course course =
        GroundBake::course(earth.world, tile.value_or(GroundBake::Tile{}), rain);
    const GroundBake::Tile on = course.begin()->first;
    const std::vector<GroundBake::Baked> written =
        GroundBake::bake(earth.generator, GroundBake::held_in(earth.store, on), on, rain, 0.5);
    // Somewhere in it water stands half a voxel deep or more.
    std::uint16_t deepest = 0;
    for (const GroundBake::Baked& baked : written) {
        if (!baked.brick) {
            continue;
        }
        hog::globe::BrickVoxels voxels{};
        baked.brick->unpack(voxels);
        for (const hog::globe::Voxel& voxel : voxels) {
            if (voxel.solid == hog::globe::Material::Air) {
                deepest = std::max(deepest, voxel.liquid_amount);
            }
        }
    }
    CHECK(deepest >= hog::globe::Voxel::full / 2);
}

TEST_CASE("springs are seeded now and then, in the tile they're for", "[server][bake]") {
    std::size_t with = 0;
    for (std::uint32_t i = 0; i < 400; ++i) {
        const GroundBake::Tile tile{.face = 1, .i = i, .j = 77};
        for (const hog::flow::Region::Spring& spring : GroundBake::springs(tile)) {
            ++with;
            const std::uint32_t across = hog::globe::brick_side * GroundBake::tile_side;
            CHECK(spring.i / across == tile.i);
            CHECK(spring.j / across == tile.j);
            CHECK(spring.discharge > 0.0);
        }
    }
    CHECK(with > 20);
    CHECK(with < 100);
}

