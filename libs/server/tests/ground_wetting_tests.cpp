#include <hog/server/ground_wetting.hpp>

#include <hog/globe/cell.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <tuple>

namespace {

using hog::DVec3;
using hog::server::GroundWetting;

struct Earth {
    hog::globe::Generator generator{hog::globe::Planet{}};
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
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

TEST_CASE("the ground around a place is taken once, a square of columns, each above its ground",
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
        const hog::globe::Layers& layers = earth().generator.layers();
        const DVec3 there = hog::globe::center(hog::globe::column_of(one.column, layers));
        CHECK(static_cast<int>(one.column.layer) >
              layers.layer_at(hog::globe::top_of(earth().world, there).value_or(0.0)));
    }
}

TEST_CASE("rain looks for the ground from over what's built on it", "[server][wetting]") {
    Earth& planet = earth();
    const DVec3 up = glm::normalize(DVec3{-0.5, 0.2, 0.84});
    const hog::globe::Cell cell = hog::globe::cell_under(up, planet.generator.layers().level);
    const DVec3 middle = hog::globe::center(cell);
    const double ground = hog::globe::top_of(planet.world, middle).value_or(0.0);
    hog::globe::build(planet.world, middle * (ground + 12.0), 2.0, hog::globe::Material::Granite);
    const std::vector<GroundWetting::Place> places{{.up = middle, .sky = {.rain = 1.0}}};
    for (const auto& one : GroundWetting::around(planet.world, places)) {
        if (one.column.i == cell.i && one.column.j == cell.j) {
            CHECK(planet.generator.layers().bottom(static_cast<int>(one.column.layer)) >
                  ground + 13.0);
        }
    }
}
