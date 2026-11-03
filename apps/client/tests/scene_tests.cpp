#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/globe_view.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/globe/generator.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/render/camera.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <test_device.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <tuple>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using hog::client::Scene;
using hog::client::Shot;
using hog::client::testing::calendar;
using hog::render::testing::test_device;

namespace {

Shot shot_of(const hog::client::Flight& flight, const hog::globe::Generator& generator) {
    const double radius = generator.planet().radius;
    const hog::DVec3 up = glm::normalize(flight.eye.metres());
    const hog::GameTime date = hog::to_game_time(
        hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}});
    return {.camera = hog::client::flight_camera(flight, 1.0),
            .bend = hog::terrain::bend_from(
                flight.eye, radius, hog::client::globe_ground(generator, flight.eye.metres())),
            .time = hog::client::start_time(calendar(), date, up, 0.5),
            .seconds = 0.0,
            .companion = std::nullopt};
}

hog::client::Forecast everywhere(hog::protocol::CellWeather cell) {
    hog::client::Forecast forecast{hog::client::testing::planet().radius};
    forecast.receive(
        {.level = 2,
         .fade = 0ms,
         .cells = std::vector<hog::protocol::CellWeather>(hog::protocol::weather_cells(2), cell)},
        hog::MonotonicClock::time_point{});
    return forecast;
}

} // namespace

TEST_CASE("the weather at the eye is clear until some is known", "[client][scene]") {
    const hog::globe::Generator& generator = hog::client::testing::globe().generator;
    Shot shot =
        shot_of(hog::client::start_flight(hog::planet::start_direction, generator.planet().radius,
                                          1'500.0, hog::planet::north_pole),
                generator);
    CHECK(hog::client::weather_at_eye(shot) == hog::protocol::CellWeather{});
    const hog::client::Forecast forecast = everywhere({.cloud = 2.0F, .falling = 4.0F});
    shot.forecast = &forecast;
    const hog::protocol::CellWeather here = hog::client::weather_at_eye(shot);
    CHECK_THAT(here.cloud, WithinAbs(2.0, 1e-3));
    CHECK_THAT(here.falling, WithinAbs(4.0, 1e-3));
}

TEST_CASE("the weather drifts along the ground, east and north as it says", "[client][scene]") {
    const hog::DVec3 up = glm::normalize(hog::DVec3{1.0, 2.0, 3.0});
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(up);
    const hog::DVec3 drift = hog::client::drift_at(up, {.drift = {3.0F, -4.0F}});
    CHECK_THAT(glm::dot(drift, frame.east), WithinAbs(3.0, 1e-9));
    CHECK_THAT(glm::dot(drift, frame.north), WithinAbs(-4.0, 1e-9));
    CHECK_THAT(glm::dot(drift, up), WithinAbs(0.0, 1e-9));
}

TEST_CASE("rain falls round the eye only under the cloud", "[client][scene]") {
    const hog::protocol::CellWeather raining{
        .cloud = 2.0F, .falling = 5.0F, .base = 1'500.0F, .top = 3'000.0F};
    hog::terrain::Bend bend{.eye_height = 1'400.0};
    CHECK(hog::client::falling_at_eye(bend, raining) == 5.0);
    bend.eye_height = 1'600.0;
    CHECK(hog::client::falling_at_eye(bend, raining) == 0.0);
}

TEST_CASE("cloud shades the eye from above, less from within it", "[client][scene]") {
    const hog::protocol::CellWeather overcast{.cloud = 2.0F, .base = 1'500.0F, .top = 3'000.0F};
    hog::terrain::Bend bend{.eye_height = 1'400.0};
    CHECK(hog::client::shade_at_eye(bend, overcast).direct < 0.01);
    CHECK(hog::client::shade_at_eye(bend, {}).direct == 1.0);
    bend.eye_height = 2'990.0;
    const double near_top = hog::client::shade_at_eye(bend, overcast).direct;
    CHECK(near_top > 0.01);
    CHECK(near_top < 1.0);
    bend.eye_height = 3'100.0;
    CHECK(hog::client::shade_at_eye(bend, overcast).direct == 1.0);
}

TEST_CASE("the globe's voxels, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const double radius = config.radius;
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    for (const char* name : {"earth", "moon"}) {
        const hog::client::GlobeWorld globe{
            hog::client::globe_planet(name, config.seed, radius).value()};
        auto scene = Scene::create(*device, globe.generator.planet(), jobs, calendar(),
                                   hog::client::still_format, 1);
        REQUIRE(scene);
        scene->follow_globe(globe.world);
        const hog::DVec3 noon = glm::normalize(hog::DVec3{1.0, 0.2, 0.3});
        const hog::DVec3 at = glm::normalize(noon + hog::DVec3{0.0, 0.0, 0.3});
        const double ground = hog::client::globe_ground(globe.generator, noon);
        for (const auto& [shot, height, pitch] :
             {std::tuple{"high", 60.0, -0.6}, std::tuple{"low", 60.0, 0.2},
              std::tuple{"1km", 1'000.0, -0.25}, std::tuple{"15km", 15'000.0, -0.45},
              std::tuple{"orbit", 400'000.0, -1.5}}) {
            hog::client::Flight flight =
                hog::client::start_flight(noon, radius, ground + height, at);
            flight.pitch = pitch;
            const auto still = hog::client::draw_still(
                *device, *scene, shot_of(flight, globe.generator), {.width = 1600, .height = 900});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("globe_{}_{}.png", name, shot), *still));
        }
    }
}

TEST_CASE("the globe at reported spots, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const double radius = hog::client::testing::planet().radius;
    const hog::client::GlobeWorld globe{
        hog::client::globe_planet(std::nullopt, hog::client::testing::planet().seed, radius)
            .value()};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, globe.generator.planet(), jobs, calendar(),
                               hog::client::still_format, 1);
    REQUIRE(scene);
    scene->follow_globe(globe.world);
    constexpr double degree = std::numbers::pi / 180.0;
    // As --at has them: latitude, longitude, metres over the radius, heading and pitch.
    struct Spot {
        const char* name;
        double latitude;
        double longitude;
        double altitude;
        double heading;
        double pitch;
    };
    for (const Spot& spot : {Spot{.name = "coast",
                                  .latitude = -31.335428,
                                  .longitude = 176.264827,
                                  .altitude = 2.0,
                                  .heading = -33.5,
                                  .pitch = -22.7},
                             Spot{.name = "cave",
                                  .latitude = -26.035949,
                                  .longitude = 164.090755,
                                  .altitude = 805.0,
                                  .heading = -149.1,
                                  .pitch = -9.7},
                             Spot{.name = "trench",
                                  .latitude = -29.298708,
                                  .longitude = -172.731839,
                                  .altitude = 1515.0,
                                  .heading = 47.9,
                                  .pitch = -7.9},
                             Spot{.name = "beds",
                                  .latitude = -29.243655,
                                  .longitude = -172.852987,
                                  .altitude = 1526.0,
                                  .heading = 14.4,
                                  .pitch = -9.9},
                             // A shelf once drawn as an island from up here, bare of its sea.
                             Spot{.name = "shelf",
                                  .latitude = -79.993368,
                                  .longitude = -132.811001,
                                  .altitude = 6536.0,
                                  .heading = -112.7,
                                  .pitch = -25.4},
                             // Tunnels reported 2026-11-01: the ground missing over a cave, a
                             // mouth cut off at a chunk, a stepped floor, a slab in a tunnel.
                             Spot{.name = "cave_over",
                                  .latitude = -6.942166,
                                  .longitude = 138.638398,
                                  .altitude = 834.0,
                                  .heading = -44.5,
                                  .pitch = -11.0},
                             Spot{.name = "cave_mouth",
                                  .latitude = -6.931845,
                                  .longitude = 138.631663,
                                  .altitude = 832.0,
                                  .heading = -178.9,
                                  .pitch = -13.2},
                             Spot{.name = "cave_floor",
                                  .latitude = -6.923761,
                                  .longitude = 138.614797,
                                  .altitude = 852.0,
                                  .heading = 7.2,
                                  .pitch = -58.6},
                             Spot{.name = "cave_slab",
                                  .latitude = -6.892869,
                                  .longitude = 138.528261,
                                  .altitude = 830.0,
                                  .heading = -54.5,
                                  .pitch = 4.9}}) {
        const hog::DVec3 up =
            hog::planet::direction_at(spot.latitude * degree, spot.longitude * degree);
        const hog::client::Flight flight = hog::client::turned_to(
            hog::client::start_flight(up, radius, spot.altitude, hog::planet::north_pole),
            spot.heading * degree, spot.pitch * degree);
        const auto still = hog::client::draw_still(
            *device, *scene, shot_of(flight, globe.generator), {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("globe_at_{}.png", spot.name), *still));
    }
}

TEST_CASE("worlds at coarser and finer cell sizes, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const double radius = config.radius;
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    const hog::DVec3 noon = glm::normalize(hog::DVec3{1.0, 0.2, 0.3});
    const hog::DVec3 at = glm::normalize(noon + hog::DVec3{0.0, 0.0, 0.3});
    for (const char* name : {"earth", "mars", "moon"}) {
        for (const double voxel : {1.0, 0.5, 0.25, 0.1, 0.05}) {
            hog::globe::Planet planet = hog::client::globe_planet(name, config.seed, radius).value();
            planet.voxel = voxel;
            const hog::client::GlobeWorld globe{planet};
            auto scene = Scene::create(*device, globe.generator.planet(), jobs, calendar(),
                                       hog::client::still_format, 1);
            REQUIRE(scene);
            scene->follow_globe(globe.world);
            const double ground = hog::client::globe_ground(globe.generator, noon);
            for (const auto& [shot, height, pitch] :
                 {std::tuple{"walk", 1.7, -0.12}, std::tuple{"low", 60.0, -0.25},
                  std::tuple{"mid", 600.0, -0.3}}) {
                hog::client::Flight flight =
                    hog::client::start_flight(noon, radius, ground + height, at);
                flight.pitch = pitch;
                const auto still = hog::client::draw_still(
                    *device, *scene, shot_of(flight, globe.generator), {.width = 1600, .height = 900});
                REQUIRE(still);
                REQUIRE(hog::client::save_picture(
                    std::filesystem::path{where} /
                        std::format("cell_{}_{}_{}.png", name, shot, std::lround(voxel * 100.0)),
                    *still));
            }
        }
    }
}

// Draws the weather a server made (the keeper's [.look] test writes it) over the globe.
TEST_CASE("the server's weather over the globe, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* made = std::getenv("HOG_WEATHER");
    REQUIRE(where != nullptr);
    REQUIRE(made != nullptr);
    std::ifstream in{made, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in}, {}};
    const auto message = hog::protocol::decode(std::as_bytes(std::span{raw.data(), raw.size()}));
    REQUIRE(message);
    const auto* weather = std::get_if<hog::protocol::Weather>(&*message);
    REQUIRE(weather != nullptr);

    const double radius = 115'000.0;
    const hog::client::GlobeWorld globe{
        hog::client::globe_planet("earth", hog::client::testing::planet().seed, radius).value()};
    hog::client::Forecast forecast{radius};
    forecast.receive(*weather, hog::MonotonicClock::time_point{});
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, globe.generator.planet(), jobs, calendar(),
                               hog::client::still_format, 1);
    REQUIRE(scene);
    scene->follow_globe(globe.world);
    for (const auto& [name, up, height, pitch] :
         {std::tuple{"ground", glm::normalize(hog::DVec3{0.6, 0.3, 0.7}), 60.0, 0.2},
          std::tuple{"tropic", glm::normalize(hog::DVec3{0.9, -0.4, 0.1}), 1'500.0, 0.15},
          std::tuple{"mid", glm::normalize(hog::DVec3{0.6, 0.3, 0.7}), 1'500.0, -0.1},
          std::tuple{"high", glm::normalize(hog::DVec3{1.0, 0.2, 0.3}), 12'000.0, -0.4},
          std::tuple{"orbit", glm::normalize(hog::DVec3{1.0, 0.2, 0.3}), 400'000.0, -1.5}}) {
        const double ground = hog::client::globe_ground(globe.generator, up * radius);
        hog::client::Flight flight = hog::client::start_flight(
            up, radius, ground + height, glm::normalize(up + hog::DVec3{0.0, 0.0, 0.3}));
        flight.pitch = pitch;
        Shot shot = shot_of(flight, globe.generator);
        shot.forecast = &forecast;
        shot.now = hog::MonotonicClock::time_point{};
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("weather_{}.png", name), *still));
    }
}

// A swell running in at the coast from each side in turn, drawn from the coast spot.
TEST_CASE("a swell on the coast, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const double radius = hog::client::testing::planet().radius;
    const hog::client::GlobeWorld globe{
        hog::client::globe_planet(std::nullopt, hog::client::testing::planet().seed, radius)
            .value()};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, globe.generator.planet(), jobs, calendar(),
                               hog::client::still_format, 1);
    REQUIRE(scene);
    scene->follow_globe(globe.world);
    constexpr double degree = std::numbers::pi / 180.0;
    const hog::DVec3 up = hog::planet::direction_at(-31.335428 * degree, 176.264827 * degree);
    const hog::client::Flight flight =
        hog::client::turned_to(hog::client::start_flight(up, radius, 6.0, hog::planet::north_pole),
                               -33.5 * degree, -10.0 * degree);
    for (const auto& [name, east_run, north_run] :
         {std::tuple{"east", 1.0F, 0.0F}, std::tuple{"west", -1.0F, 0.0F},
          std::tuple{"north", 0.0F, 1.0F}, std::tuple{"south", 0.0F, -1.0F}}) {
        hog::protocol::CellWeather cell;
        cell.wind = {east_run * 8.0F, north_run * 8.0F};
        cell.swells[0] = {.height = 2.4F, .peak = 0.7F, .heading = {east_run, north_run}};
        const auto forecast = everywhere(cell);
        Shot shot = shot_of(flight, globe.generator);
        shot.forecast = &forecast;
        shot.now = hog::MonotonicClock::time_point{};
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("swell_{}.png", name), *still));
    }
}
