#include <hog/mapgen/season_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <vector>

using namespace std::chrono;
using hog::mapgen::Image;
using hog::mapgen::Rgb;
using hog::mapgen::SeasonMap;

namespace {

constexpr Rgb grass{60, 140, 60};
constexpr Rgb river{20, 70, 200};

Image solid(Rgb colour) {
    Image image{.width = 2, .height = 1, .pixels = {}};
    for (int pixel = 0; pixel < 2; ++pixel) {
        image.pixels.insert(image.pixels.end(), colour.begin(), colour.end());
    }
    return image;
}

} // namespace

TEST_CASE("rain runs from parched tan to deep blue", "[mapgen][seasons]") {
    const Rgb dry = hog::mapgen::rain_colour(0.0);
    const Rgb wet = hog::mapgen::rain_colour(10.0);
    CHECK(dry[0] > dry[2]);
    CHECK(wet[2] > wet[0]);
    CHECK(hog::mapgen::rain_colour(-1.0) == dry);
}

TEST_CASE("soil is coloured by its water and whitens under snow", "[mapgen][seasons]") {
    using hog::mapgen::soil_colour;
    CHECK(soil_colour(0.6, 0.0) == hog::mapgen::moisture_colour(hog::planet::fixed_one * 6 / 10));
    const Rgb dry = soil_colour(0.0, 0.0);
    const Rgb wet = soil_colour(1.0, 0.0);
    CHECK(dry[0] > dry[2]);
    CHECK(wet[2] > wet[0]);
    const Rgb snowed = soil_colour(0.0, 1'000.0);
    CHECK(snowed[0] > 235);
    CHECK(snowed[2] > 235);
    const Rgb dusted = soil_colour(0.0, 5.0);
    CHECK(dusted[2] > dry[2]);
    CHECK(dusted[2] < snowed[2]);
}

TEST_CASE("rivers fade in a drought and muddy in flood", "[mapgen][seasons]") {
    using hog::mapgen::flowing_colour;
    CHECK(flowing_colour(grass, river, 1.0) == river);
    const Rgb dry = flowing_colour(grass, river, 0.0);
    CHECK(dry[1] > river[1]);
    CHECK(dry[2] < river[2]);
    CHECK(flowing_colour(grass, river, 0.5)[2] > dry[2]);
    const Rgb flood = flowing_colour(grass, river, 10.0);
    CHECK(flood[0] > flood[2]);
    CHECK(flowing_colour(grass, river, 3.0)[2] > flood[2]);
}

TEST_CASE("plants run from bare earth to grown and glow alight", "[mapgen][seasons]") {
    using hog::mapgen::forest_colour;
    CHECK(forest_colour(grass, 1.0, 0.0) == grass);
    const Rgb bare = forest_colour(grass, 0.0, 0.0);
    CHECK(bare[0] > grass[0]);
    const Rgb burning = forest_colour(grass, 1.0, 1.0);
    CHECK(burning[0] > 240);
    CHECK(burning[2] < 40);
    CHECK(forest_colour(grass, 1.0, 0.1)[0] > grass[0]);
}

TEST_CASE("seasons render over a view", "[mapgen][seasons]") {
    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    const hog::sim::WeatherConfig config{.level = 3};
    const hog::sim::CellGrid grid{config.level};
    const hog::GameTime winter{sys_days{2026y / January / 15}.time_since_epoch()};
    const hog::sim::Weather weather{config, hog::sim::ground_for(grid, world), winter};
    hog::sim::ForestConfig plants{.level = config.level};
    plants.vegetation.growth.capacity.fill(10.0);
    const hog::sim::Forest forest{plants, hog::sim::soil_for(grid, world), winter};
    hog::jobs::JobSystem jobs{1};
    for (int map = 0; map < hog::mapgen::season_map_count; ++map) {
        const auto image = hog::mapgen::render_season(world.geology(), weather, forest, 115'000.0,
                                                      hog::mapgen::PlanetView{},
                                                      static_cast<SeasonMap>(map), 64, jobs);
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
}

TEST_CASE("the surface is sampled as sea or land", "[mapgen][seasons]") {
    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    const hog::sim::WeatherConfig config{.level = 3};
    const hog::sim::CellGrid grid{config.level};
    const hog::GameTime summer{sys_days{2026y / July / 15}.time_since_epoch()};
    const hog::sim::Weather weather{config, hog::sim::ground_for(grid, world), summer};
    hog::sim::ForestConfig plants{.level = config.level};
    plants.vegetation.growth.capacity.fill(10.0);
    const hog::sim::Forest forest{plants, hog::sim::soil_for(grid, world), summer};
    const hog::mapgen::ImageSize size{.width = 64, .height = 32};
    int sea = 0;
    int land = 0;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const hog::DVec3 point =
                hog::mapgen::pixel_direction(hog::mapgen::PlanetView{}, size, x, y, 115'000.0);
            const hog::mapgen::Surface surface =
                hog::mapgen::sample_surface(world.geology(), weather, forest, point, 10'000.0);
            const bool ocean =
                world.geology().terrain().elevation(hog::planet::cell_at(point)) <= 0;
            CHECK((surface.biome == hog::planet::Biome::Ocean) == ocean);
            if (ocean) {
                ++sea;
                CHECK(surface.depth >= 0.0);
                CHECK(surface.ice >= 0.0);
                CHECK(surface.ice <= 1.0);
            } else {
                ++land;
                CHECK(surface.plants >= 0.0);
                CHECK(surface.soil >= 0.0);
                CHECK(surface.river <= 1.0);
            }
        }
    }
    CHECK(sea > 0);
    CHECK(land > 0);
}

TEST_CASE("a contact sheet lays frames out in rows", "[mapgen][seasons]") {
    const std::vector frames{solid({255, 0, 0}), solid({0, 255, 0}), solid({0, 0, 255})};
    const Image sheet = hog::mapgen::contact_sheet(frames, 2);
    CHECK(sheet.width == 4);
    CHECK(sheet.height == 2);
    CHECK(sheet.at(1, 0) == Rgb{255, 0, 0});
    CHECK(sheet.at(2, 0) == Rgb{0, 255, 0});
    CHECK(sheet.at(1, 1) == Rgb{0, 0, 255});
    CHECK(sheet.at(3, 1) == Rgb{0, 0, 0});

    const Image row = hog::mapgen::contact_sheet(frames, 5);
    CHECK(row.width == 6);
    CHECK(row.height == 1);
}

TEST_CASE("season maps are named for their view, month and map", "[mapgen][seasons]") {
    using hog::mapgen::PlanetView;
    CHECK(hog::mapgen::season_file_name(PlanetView{}, SeasonMap::Land, 2026y / July) ==
          "planet-seasons-202607-land.png");
    CHECK(hog::mapgen::season_file_name(hog::mapgen::FaceView{.face = 3}, SeasonMap::Rain,
                                        2027y / January) == "face3-seasons-202701-rain.png");
    CHECK(hog::mapgen::season_sheet_name(hog::mapgen::IslandsView{.width = 1.0},
                                         SeasonMap::Forest) == "islands-seasons-forest.png");
    std::set<std::string_view> names;
    for (int map = 0; map < hog::mapgen::season_map_count; ++map) {
        names.insert(hog::mapgen::to_string(static_cast<SeasonMap>(map)));
    }
    CHECK(names.size() == hog::mapgen::season_map_count);
}
