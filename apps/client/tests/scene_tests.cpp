#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/scene.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

using namespace std::chrono_literals;
using hog::client::Picture;
using hog::client::Scene;
using hog::client::Shot;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 160, .height = 90};

const hog::planet::PlanetConfig& planet() {
    static const hog::planet::PlanetConfig config =
        hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" /
                                        "planet.toml")
            .value();
    return config;
}

const hog::planet::World& world() {
    static const hog::planet::World made{planet()};
    return made;
}

hog::sim::Calendar calendar() {
    return {planet().day_length, planet().axial_tilt};
}

// Over the starting islands at noon, looking a little down.
Shot over_the_islands() {
    const double radius = world().geology().planet_radius();
    const auto& islands = world().geology().terrain().archipelago();
    hog::client::Flight flight = hog::client::start_flight(islands.center(), radius, 1'500.0,
                                                           islands.islands().front().center);
    flight.pitch = -0.2;
    const hog::DVec3 up = glm::normalize(flight.eye.metres());
    const hog::GameTime date = hog::to_game_time(
        hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}});
    return {.camera = hog::client::flight_camera(flight, 1.0),
            .bend = hog::terrain::bend_from(flight.eye, radius,
                                            hog::client::ground_under(world(), flight.eye)),
            .time = hog::client::start_time(calendar(), date, up, 0.5),
            .seconds = 0.0,
            .companion = std::nullopt};
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

// The mean of a row.
Rgb row(const Picture& picture, std::uint32_t y) {
    Rgb sum;
    for (std::uint32_t x = 0; x < picture.width; ++x) {
        const std::size_t at = 4 * ((std::size_t{y} * picture.width) + x);
        sum.r += std::to_integer<int>(picture.texels.at(at));
        sum.g += std::to_integer<int>(picture.texels.at(at + 1));
        sum.b += std::to_integer<int>(picture.texels.at(at + 2));
    }
    const auto width = static_cast<int>(picture.width);
    return {.r = sum.r / width, .g = sum.g / width, .b = sum.b / width};
}

} // namespace

TEST_CASE("a scene drawn as a still settles, with the sky above and the islands below",
          "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    CHECK(!scene->settled());
    const auto still = hog::client::draw_still(*device, *scene, over_the_islands(), extent);
    REQUIRE(still);
    CHECK(scene->settled());
    REQUIRE(still->width == extent.width);
    REQUIRE(still->texels.size() == std::size_t{extent.width} * extent.height * 4);
    const Rgb sky = row(*still, 0);
    const Rgb below = row(*still, extent.height - 1);
    INFO("sky " << sky.r << ", " << sky.g << ", " << sky.b);
    INFO("below " << below.r << ", " << below.g << ", " << below.b);
    CHECK(sky.b > sky.r);
    CHECK(sky.b > 100);
    CHECK(below.r + below.g + below.b > 100);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a still of ground that's still arriving fails", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    CHECK(!hog::client::draw_still(*device, *scene, over_the_islands(), extent, 1));
    CHECK(device->validation_errors() == 0);
}
