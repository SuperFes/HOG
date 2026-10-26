#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/world.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <vector>

using namespace std::chrono;
using hog::mapgen::Rgb;

namespace {

constexpr Rgb grass{60, 140, 60};
constexpr hog::GameTime new_year{sys_days{2026y / January / 1}.time_since_epoch()};

hog::sim::Ecology deer_only() {
    hog::sim::Ecology ecology;
    ecology.forage.capacity.fill(100'000.0);
    hog::sim::Species deer{.name = "red deer", .diet = {hog::sim::Food{}}, .crowded = 10.0};
    deer.habitat.fill(1.0);
    ecology.species.push_back(deer);
    return ecology;
}

} // namespace

TEST_CASE("denser wildlife shows brighter over dimmed ground", "[mapgen][wildlife]") {
    using hog::mapgen::density_colour;
    const Rgb empty = density_colour(grass, 0.0);
    CHECK(empty[1] < grass[1]);
    CHECK(density_colour(grass, -1.0) == empty);
    const Rgb few = density_colour(grass, 0.05);
    const Rgb many = density_colour(grass, 1.0);
    CHECK(few[0] > empty[0]);
    CHECK(many[0] > few[0]);
    CHECK(many[1] > few[1]);
    CHECK(density_colour(grass, 5.0) == many);
}

TEST_CASE("wildlife renders over a view", "[mapgen][wildlife]") {
    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    hog::sim::WildlifeConfig config{.level = 3};
    config.ecology = deer_only();
    const hog::sim::CellGrid grid{config.level};
    const hog::sim::Wildlife wildlife{config, hog::sim::habitat_for(grid, world), new_year};
    hog::jobs::JobSystem jobs{1};
    const auto image = hog::mapgen::render_wildlife(world.geology(), wildlife, 0, 115'000.0,
                                                    hog::mapgen::PlanetView{}, 64, jobs);
    CHECK(image.width == 64);
    CHECK(image.height == 32);
    std::set<Rgb> colours;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            colours.insert(image.at(x, y));
        }
    }
    CHECK(colours.size() > 10);
}

TEST_CASE("wildlife maps and totals are named and laid out", "[mapgen][wildlife]") {
    CHECK(hog::mapgen::wildlife_file_name(hog::mapgen::PlanetView{}, "red deer", new_year) ==
          "planet-wildlife-20260101T0000-red-deer.png");
    CHECK(hog::mapgen::wildlife_header(deer_only()) == "time,red deer,forage,carrion");

    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    hog::sim::WildlifeConfig config{.level = 2};
    config.ecology = deer_only();
    const hog::sim::CellGrid grid{config.level};
    const hog::sim::Wildlife wildlife{config, hog::sim::habitat_for(grid, world), new_year};
    const std::string row = hog::mapgen::wildlife_row(wildlife);
    CHECK(row.starts_with("2026-01-01T00:00,"));
    CHECK(row.ends_with(",100000,0.00"));
}
