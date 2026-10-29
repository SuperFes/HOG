#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>

using hog::client::Picture;
using hog::client::Scene;
using hog::client::Shot;
using hog::client::testing::calendar;
using hog::client::testing::world;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 160, .height = 90};

// 300 metres over the woods nearest the start at noon on midsummer's day, looking north and down,
// so all of the ground seen lies within the blocks' reach.
Shot over_the_woods() {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 woods =
        hog::client::testing::nearest_biome(hog::planet::Biome::TemperateForest);
    const double ground =
        std::max(hog::client::ground_under(world(), hog::WorldPosition{woods * radius}), 0.0);
    hog::client::Flight flight =
        hog::client::start_flight(woods, radius, ground + 300.0, hog::planet::north_pole);
    flight.pitch = -0.6;
    const hog::GameTime date = hog::to_game_time(
        hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}});
    return {.camera = hog::client::flight_camera(flight, 1.0),
            .bend = hog::terrain::bend_from(flight.eye, radius,
                                            hog::client::ground_under(world(), flight.eye)),
            .time =
                hog::client::start_time(calendar(), date, glm::normalize(flight.eye.metres()), 0.5),
            .seconds = 0.0,
            .companion = std::nullopt};
}

// How far apart two pictures' bottom rows are, summed over channels and texels.
int bottom_apart(const Picture& one, const Picture& other) {
    int apart = 0;
    const std::size_t first = 4 * std::size_t{extent.width} * (extent.height - 1);
    for (std::size_t at = first; at < one.texels.size(); ++at) {
        if (at % 4 != 3) {
            apart += std::abs(std::to_integer<int>(one.texels.at(at)) -
                              std::to_integer<int>(other.texels.at(at)));
        }
    }
    return apart;
}

// The bottom row's brightness, summed over channels and texels.
int bottom_brightness(const Picture& picture) {
    int sum = 0;
    const std::size_t first = 4 * std::size_t{extent.width} * (extent.height - 1);
    for (std::size_t at = first; at < picture.texels.size(); ++at) {
        if (at % 4 != 3) {
            sum += std::to_integer<int>(picture.texels.at(at));
        }
    }
    return sum;
}

} // namespace

TEST_CASE("a scene draws the ground as blocks while asked, and as before after",
          "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    CHECK_FALSE(scene->blocks());
    const Shot shot = over_the_woods();
    const auto ground = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(ground);

    scene->set_blocks(true);
    const auto blocks = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(blocks);
    CHECK(scene->settled());
    INFO("blocks bottom " << bottom_brightness(*blocks));
    CHECK(bottom_brightness(*blocks) > int{extent.width} * 60);
    const int changed = bottom_apart(*ground, *blocks);
    INFO("changed by " << changed);
    CHECK(changed > int{extent.width} * 30);

    scene->set_blocks(false);
    const auto again = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(again);
    const int returned = bottom_apart(*ground, *again);
    INFO("returned within " << returned);
    CHECK(returned < changed / 4);
    CHECK(device->validation_errors() == 0);
}
