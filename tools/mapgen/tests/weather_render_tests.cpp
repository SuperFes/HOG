#include <hog/mapgen/weather_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/world.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <vector>

using namespace std::chrono;
using hog::mapgen::Falling;
using hog::mapgen::Rgb;
using hog::mapgen::WeatherMap;

namespace {

constexpr Rgb grass{60, 140, 60};
constexpr hog::GameTime morning{(sys_days{2026y / January / 1} + hours{6}).time_since_epoch()};

} // namespace

TEST_CASE("clear skies show the ground and weather tints it", "[mapgen][weather]") {
    using hog::mapgen::sky_colour;
    CHECK(sky_colour(grass, 0.0, {}, 0.0) == grass);

    const Rgb overcast = sky_colour(grass, 1.0, {}, 0.0);
    CHECK(overcast[0] > 200);
    CHECK(overcast[1] > 200);

    const Rgb rain = sky_colour(grass, 0.0, Falling{.rain = 10.0}, 0.0);
    CHECK(rain[2] > rain[0]);
    CHECK(rain[2] > rain[1]);

    const Rgb snow = sky_colour(grass, 0.0, Falling{.snow = 10.0}, 0.0);
    CHECK(snow[0] > 240);

    const Rgb storm = sky_colour(grass, 0.0, {}, 1.0);
    CHECK(storm[0] > storm[1]);
    CHECK(storm[2] > storm[1]);

    const Rgb hail = sky_colour(grass, 0.0, Falling{.hail = 10.0}, 0.0);
    CHECK(hail[0] > 200);
    CHECK(hail[1] > 200);
    CHECK(hail[2] < 100);
}

TEST_CASE("air maps colour temperature and fronts", "[mapgen][weather]") {
    using hog::mapgen::air_colour;
    CHECK(air_colour(15.0, 0.0) == hog::mapgen::temperature_colour(15 * hog::planet::fixed_one));
    const Rgb warm = air_colour(15.0, 1.0);
    const Rgb cold = air_colour(15.0, -1.0);
    CHECK(warm[0] > warm[2]);
    CHECK(cold[2] > cold[0]);
}

TEST_CASE("weather renders over a view", "[mapgen][weather]") {
    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    const hog::sim::WeatherConfig config{.level = 3};
    const hog::sim::Weather weather{
        config, hog::sim::ground_for(hog::sim::CellGrid{config.level}, world), morning};
    hog::jobs::JobSystem jobs{1};
    for (const WeatherMap map : {WeatherMap::Sky, WeatherMap::Air}) {
        const auto image = hog::mapgen::render_weather(world.geology(), weather, 115'000.0,
                                                       hog::mapgen::PlanetView{}, map, 64, jobs);
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

TEST_CASE("weather maps are named for their view, time and map", "[mapgen][weather]") {
    CHECK(hog::mapgen::weather_file_name(hog::mapgen::PlanetView{}, WeatherMap::Sky, morning) ==
          "planet-weather-20260101T0600-sky.png");
    CHECK(hog::mapgen::weather_file_name(hog::mapgen::FaceView{.face = 2}, WeatherMap::Air,
                                         morning) == "face2-weather-20260101T0600-air.png");
    CHECK(hog::mapgen::to_string(WeatherMap::Air) == "air");
}
