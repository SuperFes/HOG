#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/ground_wetting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <tuple>

namespace {

using hog::DVec3;
using hog::server::GroundWetting;

// A direction whose ground stands well over the sea.
DVec3 land_of(const hog::globe::Generator& generator) {
    const double sea = generator.sea_level().value_or(generator.planet().body.radius);
    for (std::uint64_t k = 0;; ++k) {
        const DVec3 direction =
            hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
        if (hog::globe::ground_below(generator, direction, 0.5).value_or(0.0) > sea + 20.0) {
            return direction;
        }
    }
}

struct Earth {
    // The places the tests wet.
    Earth() {
        made.cover(glm::normalize(DVec3{0.3, 0.4, 0.86}));
        made.cover(land);
    }

    hog::globe::Generator generator{hog::globe::Planet{}};
    hog::globe::MadeWorld made{generator};
    hog::globe::World& world = made.world();
    DVec3 land = land_of(generator);
};

Earth& earth() {
    static Earth planet;
    return planet;
}

} // namespace

TEST_CASE("rain wets the ground, snow doesn't, and dry air dries it", "[server][wetting]") {
    const auto rain = GroundWetting::sky_of({.falling = 4.0F, .snow = 0.1F, .humidity = 0.9F});
    CHECK(rain.rain == 4.0);
    CHECK(rain.dryness == 0.0);
    const auto snow = GroundWetting::sky_of({.falling = 4.0F, .snow = 0.9F, .humidity = 0.9F});
    CHECK(snow.rain == 0.0);
    const auto clear = GroundWetting::sky_of({.falling = 0.0F, .snow = 0.0F, .humidity = 0.25F});
    CHECK(clear.rain == 0.0);
    CHECK(clear.dryness == 0.75);
}

TEST_CASE(
    "the ground around a place is taken once, a square of columns, each from its highest matter",
    "[server][wetting]") {
    const DVec3 up = glm::normalize(DVec3{0.3, 0.4, 0.86});
    const hog::flow::Sky sky{.rain = 3.0};
    const std::vector<GroundWetting::Place> places{{.up = up, .sky = sky}, {.up = up, .sky = {}}};
    const auto columns = GroundWetting::around(earth().world, places);
    const int side = (2 * GroundWetting::reach) + 1;
    CHECK(columns.size() <= static_cast<std::size_t>(side * side));
    CHECK(columns.size() > static_cast<std::size_t>(side * side) * 9 / 10);
    std::set<std::tuple<int, std::uint32_t, std::uint32_t>> seen;
    for (const auto& one : columns) {
        CHECK(seen.emplace(one.column.face, one.column.i, one.column.j).second);
        CHECK(one.sky.rain == 3.0);
        const auto holds = [](const hog::globe::Voxel& voxel) {
            return voxel.solid != hog::globe::Material::Air ||
                   voxel.grain != hog::globe::Material::Air ||
                   voxel.liquid != hog::globe::Material::Air;
        };
        hog::globe::Site over = one.column;
        ++over.layer;
        CHECK(holds(earth().world.at(one.column)));
        CHECK_FALSE(holds(earth().world.at(over)));
    }
}

TEST_CASE("rain looks for the ground from over what's built on it", "[server][wetting]") {
    Earth& planet = earth();
    const DVec3 up = planet.land;
    const hog::globe::Cell cell = hog::globe::cell_under(up, planet.generator.layers().level);
    const DVec3 middle = hog::globe::center(cell);
    const double ground = hog::globe::top_of(planet.world, middle).value_or(0.0);
    hog::globe::build(planet.world, middle * (ground + 12.0), 2.0, hog::globe::Material::Granite);
    const std::vector<GroundWetting::Place> places{{.up = middle, .sky = {.rain = 1.0}}};
    for (const auto& one : GroundWetting::around(planet.world, places)) {
        if (one.column.i == cell.i && one.column.j == cell.j) {
            CHECK(static_cast<int>(one.column.layer) >=
                  planet.generator.layers().layer_at(ground + 12.0));
        }
        // From where it looks, the ground is near enough to find.
        const hog::globe::Layers& layers = planet.generator.layers();
        const DVec3 there = hog::globe::center(hog::globe::column_of(one.column, layers));
        const int top = layers.layer_at(hog::globe::top_of(planet.world, there).value_or(0.0));
        CHECK(static_cast<int>(one.column.layer) - top <=
              static_cast<int>(hog::flow::Wetting::reach));
    }
}
