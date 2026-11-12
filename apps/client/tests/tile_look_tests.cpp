#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/tile_meshes.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/planet.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/tile_sending.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <numbers>
#include <set>
#include <sstream>
#include <vector>

using hog::client::testing::calendar;
using hog::render::testing::test_device;

TEST_CASE("the land as the server cuts it and the client draws it, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const hog::globe::Generator generator{*planet};
    const double radius = planet->body.radius;
    const auto device = test_device();
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t /*thread*/) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    hog::globe::Land land{generator};
    const hog::server::TileSending sending{
        hog::server::TileSending::reach_for(generator.layers().thickness)};
    constexpr double degree = std::numbers::pi / 180.0;
    // As the window's title has them: latitude, longitude, metres up, heading and pitch.
    struct Spot {
        const char* name;
        double latitude;
        double longitude;
        double up;
        double heading;
        double pitch;
    };
    std::vector<Spot> spots{Spot{.name = "coast",
                                 .latitude = -20.041310,
                                 .longitude = -95.410009,
                                 .up = 1'741.0,
                                 .heading = -25.9,
                                 .pitch = -32.0},
                            Spot{.name = "mountain",
                                 .latitude = 22.239458,
                                 .longitude = -144.433720,
                                 .up = 46.0,
                                 .heading = 19.3,
                                 .pitch = 15.7},
                            Spot{.name = "orbit",
                                 .latitude = -19.536924,
                                 .longitude = -85.287495,
                                 .up = 45'329.0,
                                 .heading = -37.4,
                                 .pitch = -85.9}};
    // A spot copied from the window's title, "latitude,longitude,up,heading,pitch", in their stead.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    if (const char* at = std::getenv("HOG_LOOK_AT")) {
        Spot spot{.name = "at",
                  .latitude = 0.0,
                  .longitude = 0.0,
                  .up = 0.0,
                  .heading = 0.0,
                  .pitch = 0.0};
        std::istringstream read{at};
        char comma = 0;
        read >> spot.latitude >> comma >> spot.longitude >> comma >> spot.up >> comma >>
            spot.heading >> comma >> spot.pitch;
        REQUIRE(read);
        spots = {spot};
    }
    for (const Spot& spot : spots) {
        const hog::DVec3 up =
            hog::planet::direction_at(spot.latitude * degree, spot.longitude * degree);
        const double ground =
            hog::globe::ground_below(generator, up, hog::globe::voxel_lod(generator))
                .value_or(radius);
        const hog::DVec3 eye = up * (ground + spot.up);
        auto scene = hog::client::Scene::create(*device, generator.planet(), jobs, calendar(),
                                                hog::client::still_format, 1);
        REQUIRE(scene);
        hog::client::TileMeshes meshes{generator.planet(), generator.layers(), jobs};
        const auto started = std::chrono::steady_clock::now();
        const hog::server::TileSending::Cut cut = sending.cut(eye, {}, land, each);
        const double cutting =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        for (const auto& tile : cut.sent) {
            REQUIRE(meshes.take(tile));
        }
        hog::client::GroundChanges changes = meshes.collect();
        meshes.finish();
        hog::client::GroundChanges more = meshes.collect();
        changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
        changes.gone.insert(changes.gone.end(), more.gone.begin(), more.gone.end());
        std::size_t quads = 0;
        std::array<std::size_t, 16> water{};
        std::array<std::size_t, 16> sea{};
        for (const auto& shown : changes.shown) {
            quads += (shown.ground->indices.size() + shown.water->indices.size()) / 6;
            const std::size_t level = shown.key >> 59U;
            water.at(level) += shown.water->indices.size() / 6;
            for (const auto& vertex : shown.water->vertices) {
                sea.at(level) += (vertex.corner & hog::client::sea_corner) != 0 ? 1 : 0;
            }
        }
        for (std::size_t level = 0; level < 8; ++level) {
            WARN(spot.name << " level " << level << ": " << water.at(level) << " water quads, "
                           << sea.at(level) / 4 << " the sea's");
        }
        const double covered = meshes.covered(eye, 8'000.0);
        WARN(spot.name << ": " << cut.sent.size() << " tiles cut in " << cutting << " s, "
                       << changes.shown.size() << " drawn, " << quads << " quads, covered "
                       << covered << " m");
        scene->show_ground(std::move(changes));
        scene->set_ground_state(covered, false);
        const hog::client::Flight flight =
            hog::client::turned_to(hog::client::start_flight(up, radius, ground + spot.up - radius,
                                                             hog::planet::north_pole),
                                   spot.heading * degree, spot.pitch * degree);
        const hog::GameTime date = hog::to_game_time(
            hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}});
        const hog::client::Shot shot{
            .camera = hog::client::flight_camera(flight, 1.0),
            .bend = hog::terrain::bend_from(flight.eye, radius, ground - radius),
            .time = hog::client::start_time(calendar(), date, up, 0.5),
            .seconds = 0.0,
            .companion = std::nullopt};
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("tiles_{}.png", spot.name), *still));
    }
}
