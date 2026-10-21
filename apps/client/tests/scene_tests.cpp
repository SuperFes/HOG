#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/cloud_quality.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <test_device.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using hog::client::Picture;
using hog::client::Scene;
using hog::client::Shot;
using hog::client::testing::calendar;
using hog::client::testing::world;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 160, .height = 90};

// Of a flight at noon on midsummer's day.
Shot shot_of(const hog::client::Flight& flight) {
    const double radius = world().geology().planet_radius();
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

// From `altitude` metres over the starting islands at noon, looking `pitch` radians up.
Shot over_the_islands(double altitude = 1'500.0, double pitch = -0.2) {
    const double radius = world().geology().planet_radius();
    const auto& islands = world().geology().terrain().archipelago();
    hog::client::Flight flight = hog::client::start_flight(islands.center(), radius, altitude,
                                                           islands.islands().front().center);
    flight.pitch = pitch;
    return shot_of(flight);
}

// Six metres over water at least five deep, looking `pitch` radians up over more of it.
Shot at_sea(double pitch) {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 centre = world().geology().terrain().archipelago().center();
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, centre));
    const auto out_to = [&](double along) {
        return glm::normalize(centre + (east * (along / radius)));
    };
    const auto deep = [&](double along) {
        return hog::client::ground_under(world(), hog::WorldPosition{out_to(along) * radius}) <
               -5.0;
    };
    double along = 0.0;
    while (!(deep(along) && deep(along + 15.0) && deep(along + 30.0))) {
        along += 250.0;
        REQUIRE(along < 200'000.0);
    }
    hog::client::Flight flight =
        hog::client::start_flight(out_to(along), radius, 6.0, out_to(along + 30.0));
    flight.pitch = pitch;
    return shot_of(flight);
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

// The same weather everywhere, shown at once.
hog::client::Forecast everywhere(hog::protocol::CellWeather cell) {
    hog::client::Forecast forecast;
    forecast.receive(
        {.level = 2,
         .fade = 0ms,
         .cells = std::vector<hog::protocol::CellWeather>(hog::protocol::weather_cells(2), cell)},
        hog::MonotonicClock::time_point{});
    return forecast;
}

int brightness(const Rgb& rgb) {
    return rgb.r + rgb.g + rgb.b;
}

} // namespace

TEST_CASE("the weather at the eye is clear until some is known", "[client][scene]") {
    Shot shot = over_the_islands();
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

TEST_CASE("a scene under rain and overcast is grey and dim", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto clear = hog::client::draw_still(*device, *scene, over_the_islands(), extent);
    REQUIRE(clear);
    const hog::client::Forecast forecast =
        everywhere({.cloud = 1.0F, .falling = 8.0F, .base = 2'000.0F, .top = 3'000.0F});
    Shot shot = over_the_islands();
    shot.forecast = &forecast;
    const auto overcast = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(overcast);
    const Rgb clear_sky = row(*clear, 0);
    const Rgb grey_sky = row(*overcast, 0);
    const Rgb clear_ground = row(*clear, extent.height - 1);
    const Rgb dim_ground = row(*overcast, extent.height - 1);
    INFO("clear sky " << clear_sky.r << ", " << clear_sky.g << ", " << clear_sky.b);
    INFO("grey sky " << grey_sky.r << ", " << grey_sky.g << ", " << grey_sky.b);
    INFO("ground " << brightness(clear_ground) << " to " << brightness(dim_ground));
    CHECK(grey_sky.b - grey_sky.r < (clear_sky.b - clear_sky.r) / 2);
    CHECK(brightness(dim_ground) < brightness(clear_ground));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a scene draws its clouds at each quality alike", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    CHECK(scene->cloud_quality() == hog::render::CloudQuality::High);
    const hog::client::Forecast forecast =
        everywhere({.cloud = 1.0F, .falling = 8.0F, .base = 2'000.0F, .top = 3'000.0F});
    Shot shot = over_the_islands();
    shot.forecast = &forecast;
    const auto high = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(high);
    REQUIRE(scene->set_cloud_quality(hog::render::CloudQuality::Low));
    CHECK(scene->cloud_quality() == hog::render::CloudQuality::Low);
    const auto low = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(low);
    const Rgb high_sky = row(*high, 0);
    const Rgb low_sky = row(*low, 0);
    INFO("high " << high_sky.r << ", " << high_sky.g << ", " << high_sky.b);
    INFO("low " << low_sky.r << ", " << low_sky.g << ", " << low_sky.b);
    CHECK(std::abs(brightness(low_sky) - brightness(high_sky)) < brightness(high_sky) / 10);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a scene in fog is grey, hiding the islands", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto clear =
        hog::client::draw_still(*device, *scene, over_the_islands(300.0, 0.0), extent);
    REQUIRE(clear);
    const hog::client::Forecast forecast = everywhere({.humidity = 1.0F, .fog_top = 1'000.0F});
    Shot shot = over_the_islands(300.0, 0.0);
    shot.forecast = &forecast;
    const auto fogged = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(fogged);
    const auto spread = [](const Rgb& rgb) {
        return std::max({rgb.r, rgb.g, rgb.b}) - std::min({rgb.r, rgb.g, rgb.b});
    };
    const Rgb ground = row(*clear, extent.height - 1);
    const Rgb grey = row(*fogged, extent.height - 1);
    INFO("ground " << ground.r << ", " << ground.g << ", " << ground.b);
    INFO("grey " << grey.r << ", " << grey.g << ", " << grey.b);
    CHECK(spread(grey) < 15);
    CHECK(spread(grey) < spread(ground) / 2);
    CHECK(brightness(grey) > 300);
    CHECK(device->validation_errors() == 0);
}

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

TEST_CASE("a scene shows someone wading in the sea through the water over them",
          "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    // Looking down at someone ten metres off, standing a metre deep.
    Shot shot = at_sea(-0.5);
    const auto alone = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(alone);
    const hog::WorldPosition surface = ahead(shot, 10.0);
    const hog::DVec3 up = glm::normalize(surface.metres());
    shot.others = {{.feet = surface - (up * 1.0), .up = up, .colour = {0.6F, 0.0F, 0.0F}}};
    const auto seen = hog::client::draw_still(*device, *scene, shot, extent);
    REQUIRE(seen);
    const hog::DVec2 under = drawn_at(shot, surface - (up * 0.5));
    const Rgb water = pixel(*alone, under);
    const Rgb wet = pixel(*seen, under);
    const Rgb dry = pixel(*seen, drawn_at(shot, surface + (up * 0.5)));
    INFO("water " << water.r << ", " << water.g << ", " << water.b);
    INFO("wet " << wet.r << ", " << wet.g << ", " << wet.b);
    INFO("dry " << dry.r << ", " << dry.g << ", " << dry.b);
    // Not hidden by the surface, but dimmed by the water above.
    CHECK(wet.r - wet.g > water.r - water.g + 15);
    CHECK(wet.r < dry.r - 15);
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
    // From this high no tree is near enough to grow, so the forest pass isn't added at all.
    CHECK(names == std::vector<std::string_view>{"uploads", "sky tables", "sky", "ground", "trees",
                                                 "sea", "glare"});
    // Over the islands' woods, the stamps draw trees; from this high, none are grown.
    CHECK(scene->tree_counts().tiles > 0);
    CHECK(scene->tree_counts().stamped > 0);
    CHECK(scene->tree_counts().grown == 0);
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

// Not a check: stills of a windy sea, from near and far and against the sun, to look at. Saved
// where HOG_LOOK names.
TEST_CASE("a windy sea, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::client::Forecast forecast = everywhere({.drift = {16.0F, 6.0F}});
    struct Look {
        std::string_view name;
        double pitch;
        double time_of_day;
    };
    for (const Look& look : {Look{.name = "down", .pitch = -0.6, .time_of_day = 0.5},
                             Look{.name = "out", .pitch = -0.12, .time_of_day = 0.5},
                             Look{.name = "level", .pitch = 0.0, .time_of_day = 0.5},
                             Look{.name = "evening", .pitch = -0.05, .time_of_day = 0.64},
                             Look{.name = "shore", .pitch = -0.1, .time_of_day = 0.5}}) {
        Shot shot = look.name == "shore" ? over_the_islands(20.0, look.pitch) : at_sea(look.pitch);
        const hog::DVec3 up = glm::normalize(shot.camera.eye.metres());
        shot.time =
            hog::client::start_time(calendar(),
                                    hog::to_game_time(hog::WallClock::time_point{
                                        std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}}),
                                    up, look.time_of_day);
        shot.forecast = &forecast;
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("{}.png", look.name), *still));
    }
}
