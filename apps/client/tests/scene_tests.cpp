#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/scene.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using hog::client::Picture;
using hog::client::Scene;
using hog::client::Shot;
using hog::client::testing::calendar;
using hog::client::testing::world;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 160, .height = 90};

// From `altitude` metres over the starting islands at noon, looking `pitch` radians up.
Shot over_the_islands(double altitude = 1'500.0, double pitch = -0.2) {
    const double radius = world().geology().planet_radius();
    const auto& islands = world().geology().terrain().archipelago();
    hog::client::Flight flight = hog::client::start_flight(islands.center(), radius, altitude,
                                                           islands.islands().front().center);
    flight.pitch = pitch;
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

Rgb pixel(const Picture& picture, hog::DVec2 at) {
    const std::size_t first =
        4 * ((static_cast<std::size_t>(at.y) * picture.width) + static_cast<std::size_t>(at.x));
    return {.r = std::to_integer<int>(picture.texels.at(first)),
            .g = std::to_integer<int>(picture.texels.at(first + 1)),
            .b = std::to_integer<int>(picture.texels.at(first + 2))};
}

// Where in the picture a point is drawn.
hog::DVec2 drawn_at(const Shot& shot, hog::WorldPosition at) {
    const hog::DVec2 size{extent.width, extent.height};
    const glm::vec4 clip =
        hog::render::view_projection(shot.camera, size.x / size.y) *
        glm::vec4{hog::Vec3{hog::terrain::bent(shot.bend, at - shot.camera.eye)}, 1.0F};
    return ((hog::DVec2{clip.x, clip.y} / static_cast<double>(clip.w)) + 1.0) / 2.0 * size;
}

// On the ground or the sea `along` metres ahead of the shot's eye.
hog::WorldPosition ahead(const Shot& shot, double along) {
    const hog::DVec3 up = glm::normalize(shot.camera.eye.metres());
    const hog::DVec3 level =
        glm::normalize(shot.camera.forward - (up * glm::dot(shot.camera.forward, up)));
    const hog::DVec3 there = glm::normalize((shot.camera.eye + (level * along)).metres());
    const double radius = world().geology().planet_radius();
    return hog::WorldPosition{
        there *
        (radius +
         std::max(hog::client::ground_under(world(), hog::WorldPosition{there * radius}), 0.0))};
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

TEST_CASE("a scene draws others where they stand", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    // Standing two metres over the ground, with someone fifteen metres ahead.
    const auto& islands = world().geology().terrain().archipelago();
    const double ground = std::max(
        hog::client::ground_under(
            world(), hog::WorldPosition{islands.center() * world().geology().planet_radius()}),
        0.0);
    Shot shot = over_the_islands(ground + 2.0, 0.0);
    const auto alone = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(alone);
    const hog::WorldPosition feet = ahead(shot, 15.0);
    const hog::DVec3 up = glm::normalize(feet.metres());
    shot.others = {{.feet = feet, .up = up, .colour = {0.6F, 0.0F, 0.0F}}};
    const auto seen = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(seen);
    const hog::DVec2 middle = drawn_at(shot, feet + (up * 1.0));
    const Rgb before = pixel(*alone, middle);
    const Rgb after = pixel(*seen, middle);
    INFO("before " << before.r << ", " << before.g << ", " << before.b);
    INFO("after " << after.r << ", " << after.g << ", " << after.b);
    CHECK(after.r - after.g > before.r - before.g + 30);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a scene times its passes on the GPU", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    if (device->timestamp_bits() == 0) {
        SKIP("the Vulkan device's queue can't time");
    }
    // Over more than one frame, so one has been read back.
    REQUIRE(hog::client::draw_still(*device, *scene, over_the_islands(), extent));
    std::vector<std::string_view> names;
    for (const hog::render::GpuSpan& span : scene->timings()) {
        names.push_back(span.name);
        CHECK(span.milliseconds >= 0.0);
    }
    CHECK(names ==
          std::vector<std::string_view>{"uploads", "sky tables", "sky", "ground", "sea", "glare"});
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
