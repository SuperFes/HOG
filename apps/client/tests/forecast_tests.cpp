#include <hog/client/forecast.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/weather.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::MonotonicClock;
using hog::client::cube_direction;
using hog::client::Forecast;
using hog::client::weather_map_size;
using hog::protocol::CellWeather;
using hog::protocol::Weather;

namespace {

constexpr MonotonicClock::time_point start{};

Weather everywhere(CellWeather cell, std::chrono::milliseconds fade = 10s, std::uint8_t level = 2) {
    return {.level = level,
            .fade = fade,
            .cells = std::vector<CellWeather>(hog::protocol::weather_cells(level), cell)};
}

double cloud_at(const Forecast& forecast, MonotonicClock::time_point now) {
    return static_cast<double>(forecast.at(glm::normalize(DVec3{1.0, 2.0, 3.0}), now).cloud);
}

// Where Vulkan samples a direction: its face, and s and t across it from 0 to 1.
struct Sampled {
    std::uint32_t face;
    double s;
    double t;
};

Sampled sampled(DVec3 d) {
    const DVec3 a = glm::abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        return d.x > 0.0 ? Sampled{.face = 0,
                                   .s = ((-d.z / a.x) + 1.0) / 2.0,
                                   .t = ((-d.y / a.x) + 1.0) / 2.0}
                         : Sampled{.face = 1,
                                   .s = ((d.z / a.x) + 1.0) / 2.0,
                                   .t = ((-d.y / a.x) + 1.0) / 2.0};
    }
    if (a.y >= a.z) {
        return d.y > 0.0 ? Sampled{.face = 2,
                                   .s = ((d.x / a.y) + 1.0) / 2.0,
                                   .t = ((d.z / a.y) + 1.0) / 2.0}
                         : Sampled{.face = 3,
                                   .s = ((d.x / a.y) + 1.0) / 2.0,
                                   .t = ((-d.z / a.y) + 1.0) / 2.0};
    }
    return d.z > 0.0
               ? Sampled{.face = 4, .s = ((d.x / a.z) + 1.0) / 2.0, .t = ((-d.y / a.z) + 1.0) / 2.0}
               : Sampled{
                     .face = 5, .s = ((-d.x / a.z) + 1.0) / 2.0, .t = ((-d.y / a.z) + 1.0) / 2.0};
}

} // namespace

TEST_CASE("a cube map's texels look the way Vulkan samples them", "[client][forecast]") {
    constexpr std::uint32_t size = 8;
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                CAPTURE(face, x, y);
                const DVec3 direction = cube_direction(face, x, y, size);
                CHECK_THAT(glm::length(direction), WithinAbs(1.0, 1e-12));
                const Sampled at = sampled(direction);
                CHECK(at.face == face);
                CHECK_THAT(at.s * size, WithinAbs(x + 0.5, 1e-9));
                CHECK_THAT(at.t * size, WithinAbs(y + 0.5, 1e-9));
            }
        }
    }
}

TEST_CASE("the weather reads as a line", "[client][forecast]") {
    CHECK(hog::client::describe({}) == "0% cloud, 0% humid, drifting 0 m/s");
    CHECK(hog::client::describe({.cloud = 16.0F,
                                 .falling = 2.5F,
                                 .drift = {3.0F, -4.0F},
                                 .humidity = 0.92F,
                                 .fog_top = 300.0F,
                                 .base = 800.0F,
                                 .top = 2'100.0F}) ==
          "100% cloud at 800-2100 m, rain 2.5 mm/h, 92% humid, fog to 300 m, drifting 5 m/s");
    CHECK(hog::client::describe({.cloud = 16.0F, .falling = 1.0F, .snow = 0.75F}) ==
          "100% cloud at 0-0 m, snow 1.0 mm/h, 0% humid, drifting 0 m/s");
}

TEST_CASE("with no weather the sky is clear", "[client][forecast]") {
    const Forecast forecast;
    CHECK_FALSE(forecast.known());
    CHECK(forecast.at(DVec3{0.0, 0.0, 1.0}, start) == CellWeather{});
    CHECK(forecast.before().empty());
    CHECK(forecast.received() == 0);
}

TEST_CASE("the first weather is shown at once", "[client][forecast]") {
    Forecast forecast;
    const CellWeather rainy{.cloud = 2.0F,
                            .falling = 4.0F,
                            .snow = 0.25F,
                            .drift = {3.0F, -1.0F},
                            .humidity = 0.6F,
                            .fog_top = 320.0F,
                            .base = 1'280.0F,
                            .top = 3'200.0F};
    forecast.receive(everywhere(rainy), start);
    CHECK(forecast.known());
    CHECK(forecast.received() == 1);
    CHECK(forecast.faded(start) == 1.0);
    const CellWeather here = forecast.at(glm::normalize(DVec3{-1.0, 0.5, 0.2}), start);
    CHECK_THAT(static_cast<double>(here.cloud), WithinAbs(2.0, 1e-5));
    CHECK_THAT(static_cast<double>(here.falling), WithinAbs(4.0, 1e-5));
    CHECK_THAT(static_cast<double>(here.snow), WithinAbs(0.25, 1e-6));
    CHECK_THAT(static_cast<double>(here.drift.x), WithinAbs(3.0, 1e-5));
    CHECK_THAT(static_cast<double>(here.drift.y), WithinAbs(-1.0, 1e-5));
    REQUIRE(forecast.before().size() == std::size_t{6} * weather_map_size * weather_map_size);
    CHECK(forecast.after() == forecast.before());
    const auto cover =
        static_cast<int>(std::lround(static_cast<double>(hog::sim::cloud_cover(2.0F)) * 255.0));
    // The roots of 2/16 and of 4/256, and a quarter.
    const hog::client::WeatherTexel expected{static_cast<std::uint8_t>(cover), 90, 32, 64};
    CHECK(std::ranges::all_of(forecast.before(),
                              [&](const auto& texel) { return texel == expected; }));
    CHECK_THAT(static_cast<double>(here.humidity), WithinAbs(0.6, 1e-6));
    CHECK_THAT(static_cast<double>(here.fog_top), WithinAbs(320.0, 1e-3));
    CHECK_THAT(static_cast<double>(here.base), WithinAbs(1'280.0, 1e-3));
    CHECK_THAT(static_cast<double>(here.top), WithinAbs(3'200.0, 1e-3));
    REQUIRE(forecast.sky_before().size() == forecast.before().size());
    CHECK(forecast.sky_after() == forecast.sky_before());
    // In steps of 32 and 64 metres.
    const hog::client::SkyTexel sky{153, 10, 20, 50};
    CHECK(std::ranges::all_of(forecast.sky_before(),
                              [&](const auto& texel) { return texel == sky; }));
}

TEST_CASE("new weather fades in over its fade", "[client][forecast]") {
    Forecast forecast;
    forecast.receive(everywhere({}), start);
    forecast.receive(everywhere({.cloud = 4.0F}), start + 1s);
    CHECK(forecast.received() == 2);
    CHECK(cloud_at(forecast, start + 1s) == 0.0);
    CHECK_THAT(cloud_at(forecast, start + 6s), WithinAbs(2.0, 1e-5));
    CHECK_THAT(cloud_at(forecast, start + 11s), WithinAbs(4.0, 1e-5));
    CHECK_THAT(cloud_at(forecast, start + 1h), WithinAbs(4.0, 1e-5));
    CHECK(forecast.before().front()[0] == 0);
    CHECK(forecast.after().front()[0] > 200);
}

TEST_CASE("weather coming mid-fade fades on from what's shown", "[client][forecast]") {
    Forecast forecast;
    forecast.receive(everywhere({}), start);
    forecast.receive(everywhere({.cloud = 4.0F}), start);
    forecast.receive(everywhere({.cloud = 0.0F}), start + 5s);
    CHECK_THAT(cloud_at(forecast, start + 5s), WithinAbs(2.0, 1e-5));
    CHECK_THAT(cloud_at(forecast, start + 10s), WithinAbs(1.0, 1e-5));
    CHECK_THAT(cloud_at(forecast, start + 15s), WithinAbs(0.0, 1e-5));
}

TEST_CASE("weather with no fade, or at another level, is shown at once", "[client][forecast]") {
    Forecast forecast;
    forecast.receive(everywhere({}), start);
    forecast.receive(everywhere({.cloud = 4.0F}, 0ms), start);
    CHECK_THAT(cloud_at(forecast, start), WithinAbs(4.0, 1e-5));
    CHECK(forecast.after() == forecast.before());
    forecast.receive(everywhere({.cloud = 1.0F}, 10s, 3), start);
    CHECK_THAT(cloud_at(forecast, start), WithinAbs(1.0, 1e-5));
}

TEST_CASE("the sea goes on from where it stood when the next weather comes", "[client][forecast]") {
    const CellWeather wavy{
        .wind_sea = {.height = 2.0F, .peak = 0.8F, .heading = {1.0F, 0.0F}},
        .swells = {hog::protocol::CellWaves{.height = 1.0F, .peak = 0.5F, .heading = {0.0F, 1.0F}},
                   hog::protocol::CellWaves{}},
        .current = {0.5F, 0.0F}};
    const auto variance = [](const hog::waves::SeaState& sea) {
        double sum = 0.0;
        for (const hog::waves::System& system : sea.systems) {
            sum += system.height * system.height;
        }
        return sum;
    };
    const DVec3 at = glm::normalize(DVec3{1.0, 2.0, 0.5});
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
    const auto stood = [&](MonotonicClock::time_point when, double expected, double current) {
        Forecast forecast;
        forecast.receive(everywhere({}, 10s, hog::protocol::most_weather_level), start);
        forecast.receive(everywhere(wavy, 10s, hog::protocol::most_weather_level), start);
        const hog::waves::SeaState before = forecast.sea_at(at, when);
        forecast.receive(everywhere(wavy, 10s, hog::protocol::most_weather_level), when);
        const hog::waves::SeaState after = forecast.sea_at(at, when);
        CHECK_THAT(variance(before), WithinAbs(expected, 1e-5));
        CHECK_THAT(variance(after), WithinAbs(expected, 1e-5));
        CHECK(glm::length(after.current - (frame.east * current)) < 0.02);
    };
    stood(start + 10s, 5.0, 0.5);
    stood(start + 5s, 2.5, 0.25);
}

TEST_CASE("the sea is the waves of the cells around, each as much as it counts there",
          "[client][forecast]") {
    const CellWeather wavy{
        .wind = {0.0F, 12.0F},
        .wind_sea = {.height = 2.0F, .peak = 0.8F, .heading = {1.0F, 0.0F}},
        .swells = {hog::protocol::CellWaves{.height = 1.0F, .peak = 0.5F, .heading = {0.0F, 1.0F}},
                   hog::protocol::CellWaves{}},
        .current = {0.5F, 0.0F}};
    const auto variance = [](const hog::waves::SeaState& sea) {
        double sum = 0.0;
        for (const hog::waves::System& system : sea.systems) {
            sum += system.height * system.height;
        }
        return sum;
    };
    Forecast forecast;
    CHECK(forecast.sea_at(DVec3{0.0, 0.0, 1.0}, start).systems.empty());
    forecast.receive(everywhere({}, 10s, hog::protocol::most_weather_level), start);
    CHECK(forecast.sea_at(DVec3{0.0, 0.0, 1.0}, start).systems.empty());
    forecast.receive(everywhere(wavy, 10s, hog::protocol::most_weather_level), start);
    const DVec3 at = glm::normalize(DVec3{1.0, 2.0, 0.5});
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
    const hog::waves::SeaState half = forecast.sea_at(at, start + 5s);
    CHECK_THAT(variance(half), WithinAbs(5.0 / 2.0, 1e-9));
    const hog::waves::SeaState sea = forecast.sea_at(at, start + 10s);
    CHECK_THAT(variance(sea), WithinAbs(5.0, 1e-9));
    REQUIRE(!sea.systems.empty());
    for (const hog::waves::System& system : sea.systems) {
        CHECK_THAT(glm::length(system.heading), WithinAbs(1.0, 1e-9));
        if (system.peak == static_cast<double>(0.8F)) {
            CHECK(glm::dot(system.heading, frame.east) > 0.98);
            CHECK(system.peakedness == hog::waves::wind_sea_peakedness);
        } else {
            CHECK(system.peak == static_cast<double>(0.5F));
            CHECK(glm::dot(system.heading, frame.north) > 0.98);
            CHECK(system.spread == hog::waves::swell_spread);
        }
    }
    CHECK(glm::length(sea.current - (frame.east * 0.5)) < 0.02);
    CHECK(glm::length(sea.wind - (frame.north * 12.0)) < 0.5);
    CHECK(glm::length(forecast.at(at, start + 10s).wind - hog::Vec2{0.0F, 12.0F}) < 1e-4F);
}

TEST_CASE("the weather's map holds what's at each texel's direction", "[client][forecast]") {
    // Cloud from none to plenty about the planet, and rain where there's most.
    const hog::sim::CellGrid grid{2};
    Weather weather = everywhere({}, 0ms);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const DVec3 up = grid.frame(index).up;
        weather.cells[index].cloud = static_cast<float>(1.5 + up.x + (0.5 * up.z));
        weather.cells[index].falling = up.y > 0.5 ? 20.0F : 0.0F;
        weather.cells[index].base = static_cast<float>(2'000.0 + (1'000.0 * up.z));
        weather.cells[index].fog_top = up.x > 0.0 ? static_cast<float>(400.0 * up.x) : 0.0F;
    }
    Forecast forecast;
    forecast.receive(weather, start);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < weather_map_size; y += 7) {
            for (std::uint32_t x = 0; x < weather_map_size; x += 7) {
                CAPTURE(face, x, y);
                const auto& texel =
                    forecast.before()[(((face * weather_map_size) + y) * weather_map_size) + x];
                const CellWeather here =
                    forecast.at(cube_direction(face, x, y, weather_map_size), start);
                CHECK(std::abs(texel[0] - (hog::sim::cloud_cover(here.cloud) * 255.0F)) <= 0.51F);
                CHECK(std::abs(texel[2] - (std::sqrt(here.falling / 256.0F) * 255.0F)) <= 0.51F);
                const auto& sky =
                    forecast.sky_before()[(((face * weather_map_size) + y) * weather_map_size) + x];
                CHECK(std::abs(sky[1] - (here.fog_top / 32.0F)) <= 0.51F);
                CHECK(std::abs(sky[2] - (here.base / 64.0F)) <= 0.51F);
            }
        }
    }
}
