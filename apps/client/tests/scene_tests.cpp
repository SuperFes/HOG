#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/globe_view.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/views.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/places.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/cloud_quality.hpp>
#include <hog/render/gpu_timer.hpp>
#include <hog/render/readback.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/river_water.hpp>
#include <hog/terrain/shore.hpp>

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
#include <map>
#include <numbers>
#include <optional>
#include <string_view>
#include <tuple>
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

// The woods nearest the start, wherever the seed put them.
hog::DVec3 woods() {
    static const hog::DVec3 found =
        hog::client::testing::nearest_biome(hog::planet::Biome::TemperateForest);
    return found;
}

// From `altitude` metres over the woods at noon, looking north and `pitch` radians up.
Shot over_the_woods(double altitude = 1'500.0, double pitch = -0.2) {
    const double radius = world().geology().planet_radius();
    hog::client::Flight flight =
        hog::client::start_flight(woods(), radius, altitude, hog::planet::north_pole);
    flight.pitch = pitch;
    return shot_of(flight);
}

Shot on_the_island(double altitude, double pitch) {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 middle = hog::client::testing::nearest_biome(hog::planet::Biome::Grassland);
    const auto tall = [&](hog::DVec3 point) {
        const auto biome = world().state(hog::planet::cell_at(glm::normalize(point))).biome;
        return biome == hog::planet::Biome::Grassland || biome == hog::planet::Biome::Savanna ||
               biome == hog::planet::Biome::Wetland;
    };
    std::map<int, int> seen;
    {
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(middle);
        const hog::DVec3 outward = frame.north;
        const hog::DVec3 aside = frame.east;
        for (int a = -40; a <= 40; ++a) {
            for (int b = -40; b <= 40; ++b) {
                const hog::DVec3 there = glm::normalize(
                    middle +
                    (((outward * static_cast<double>(a)) + (aside * static_cast<double>(b))) *
                     (150.0 / radius)));
                ++seen[static_cast<int>(world().state(hog::planet::cell_at(there)).biome)];
                bool all = tall(there);
                for (const double off : {-150.0, 150.0}) {
                    all = all && tall(there + (outward * (off / radius))) &&
                          tall(there + (aside * (off / radius)));
                }
                if (all &&
                    hog::client::ground_under(world(), hog::WorldPosition{there * radius}) > 3.0) {
                    const double ground =
                        hog::client::ground_under(world(), hog::WorldPosition{there * radius});
                    hog::client::Flight flight = hog::client::start_flight(
                        there, radius, ground + altitude,
                        glm::normalize(there + (outward * (1'000.0 / radius))));
                    flight.pitch = pitch;
                    return shot_of(flight);
                }
            }
        }
    }
    for (const auto& [biome, count] : seen) {
        WARN("biome " << biome << ": " << count);
    }
    FAIL("no tall grass");
    return {};
}

// `altitude` metres over the sea `out` metres off the coast nearest the start, looking back at the
// land if `back`, else out to sea, `pitch` radians up.
Shot off_the_coast(double out, double altitude, bool back, double pitch) {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 land = world().geology().terrain().nearest_land(hog::planet::start_direction);
    const hog::DVec3 outward = glm::normalize(
        hog::planet::start_direction - (land * glm::dot(hog::planet::start_direction, land)));
    const auto at = [&](double along) {
        return glm::normalize(land + (outward * (along / radius)));
    };
    double coast = 0.0;
    while (hog::client::ground_under(world(), hog::WorldPosition{at(coast) * radius}) > 0.0) {
        coast += 5.0;
    }
    hog::client::Flight flight = hog::client::start_flight(at(coast + out), radius, altitude,
                                                           back ? land : at(coast + out + 1'000.0));
    flight.pitch = pitch;
    return shot_of(flight);
}

// Six metres over water at least five deep, looking `pitch` radians up over more of it.
Shot at_sea(double pitch) {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 centre = hog::planet::start_direction;
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

// Of the pixels in the row through `at`, up to `reach` either side, the reddest: across a figure
// only pixels wide, its sunlit side rather than whichever its axis falls on.
Rgb reddest(const Picture& picture, hog::DVec2 at, int reach = 3) {
    Rgb found = pixel(picture, at);
    for (int step = -reach; step <= reach; ++step) {
        const Rgb next = pixel(picture, at + hog::DVec2{step, 0.0});
        if (next.r - next.g > found.r - found.g) {
            found = next;
        }
    }
    return found;
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

// Ground below the sea ringed by land some hundreds of metres round, at most `most` of it, sampled
// evenly over the planet a few hundred metres apart, as Fibonacci's lattice spreads them.
std::vector<hog::DVec3> ringed_lows(std::size_t most) {
    const double radius = world().geology().planet_radius();
    const hog::planet::Elevation& terrain = world().geology().terrain();
    const auto below_sea = [&](hog::DVec3 at) {
        return hog::planet::fixed_to_double(terrain.elevation(hog::planet::cell_at(at))) < 0.0;
    };
    constexpr int samples = 2'000'000;
    constexpr double ring = 600.0; // metres
    std::vector<hog::DVec3> lows;
    for (int i = 0; i < samples && lows.size() < most; ++i) {
        const double z = 1.0 - ((2.0 * (i + 0.5)) / samples);
        const double turned = i * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const double across = std::sqrt(1.0 - (z * z));
        const hog::DVec3 at{across * std::cos(turned), across * std::sin(turned), z};
        if (!below_sea(at)) {
            continue;
        }
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
        bool ringed = true;
        for (int k = 0; k < 8 && ringed; ++k) {
            const double way = k * std::numbers::pi / 4.0;
            ringed = !below_sea(glm::normalize(
                at + (((frame.east * std::cos(way)) + (frame.north * std::sin(way))) *
                      (ring / radius))));
        }
        if (ringed) {
            lows.push_back(at);
        }
    }
    return lows;
}

// Of the still water the shore's grids round `around` find, cut off from the sea, the middle of
// the widest: the most fetch it has whichever way the wind blows, and where it lies.
std::pair<double, hog::DVec3> widest_still_water(hog::DVec3 around) {
    using hog::terrain::shore_size;
    using hog::terrain::shore_spacing;
    constexpr auto texels = static_cast<std::size_t>(shore_size) * shore_size;
    const double radius = world().geology().planet_radius();
    const hog::terrain::ShoreFrame frame = hog::terrain::shore_frame(around, radius);
    const hog::terrain::ShoreDepths shore = hog::terrain::work_out_shore(world(), frame, radius);
    const std::vector<float>& still = shore.still;
    const std::vector<float>& fetches = shore.fetches;
    std::pair<double, hog::DVec3> widest{0.0, around};
    for (std::size_t in_layers = 0; in_layers < still.size(); ++in_layers) {
        const std::size_t layer = in_layers / texels;
        const std::size_t at = in_layers % texels;
        float least = still[in_layers] > 0.0F ? hog::terrain::longest_fetch : 0.0F;
        for (std::size_t heading = 0; heading < hog::terrain::fetch_headings; ++heading) {
            least = std::min(
                least, fetches[(((layer * hog::terrain::fetch_headings) + heading) * texels) + at]);
        }
        if (static_cast<double>(least) > widest.first) {
            const double spacing = shore_spacing.at(layer);
            const double first = (0.5 - (shore_size / 2.0)) * spacing;
            const std::size_t row = at / shore_size;
            const std::size_t column = at % shore_size;
            widest = {
                static_cast<double>(least),
                hog::terrain::shore_direction(frame, radius,
                                              {first + (static_cast<double>(column) * spacing),
                                               first + (static_cast<double>(row) * spacing)})};
        }
    }
    return widest;
}

// The middle of the widest still water cut off from the sea anywhere, of the first few found.
std::optional<hog::DVec3> still_water() {
    std::pair<double, hog::DVec3> widest{0.0, hog::DVec3{0.0}};
    for (const hog::DVec3& low : ringed_lows(24)) {
        if (const auto found = widest_still_water(low); found.first > widest.first) {
            widest = found;
        }
    }
    if (widest.first <= 0.0) {
        return std::nullopt;
    }
    return widest.second;
}

// `altitude` metres over `under`, looking north `pitch` radians up.
Shot looking_north(hog::DVec3 under, double altitude, double pitch) {
    const double radius = world().geology().planet_radius();
    const hog::DVec3 pole{0.0, 0.0, 1.0};
    const hog::DVec3 ahead = glm::normalize(pole - (under * glm::dot(under, pole)));
    hog::client::Flight flight = hog::client::start_flight(
        under, radius, altitude, glm::normalize(under + (ahead * (1'000.0 / radius))));
    flight.pitch = pitch;
    return shot_of(flight);
}

// The same weather everywhere, shown at once.
hog::client::Forecast everywhere(hog::protocol::CellWeather cell) {
    hog::client::Forecast forecast{world().geology().planet_radius()};
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
    Shot shot = over_the_woods();
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
    const auto clear = hog::client::draw_still(*device, *scene, over_the_woods(), extent);
    REQUIRE(clear);
    const hog::client::Forecast forecast =
        everywhere({.cloud = 1.0F, .falling = 8.0F, .base = 2'000.0F, .top = 3'000.0F});
    Shot shot = over_the_woods();
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
    Shot shot = over_the_woods();
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

TEST_CASE("a scene in fog is grey, hiding the ground", "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto clear = hog::client::draw_still(*device, *scene, over_the_woods(300.0, 0.0), extent);
    REQUIRE(clear);
    const hog::client::Forecast forecast = everywhere({.humidity = 1.0F, .fog_top = 1'000.0F});
    Shot shot = over_the_woods(300.0, 0.0);
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

TEST_CASE("a still from under the sea takes a probe of what's out of it, and one over it doesn't",
          "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    // Each straight after the other, so the last frame of one found the eye where the other's
    // first frame doesn't have it.
    for (const auto& [altitude, under] :
         {std::pair{-3.0, true}, std::pair{6.0, false}, std::pair{-3.0, true}}) {
        INFO("at " << altitude);
        const auto still = hog::client::draw_still(
            *device, *scene, off_the_coast(120.0, altitude, true, 0.7), extent);
        REQUIRE(still);
        CHECK(scene->settled());
        CHECK(scene->probing() == under);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a scene drawn as a still settles, with the sky above and the ground below",
          "[client][scene][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    CHECK(!scene->settled());
    const auto still = hog::client::draw_still(*device, *scene, over_the_woods(), extent);
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
    const double ground =
        std::max(hog::client::ground_under(
                     world(), hog::WorldPosition{woods() * world().geology().planet_radius()}),
                 0.0);
    Shot shot = over_the_woods(ground + 2.0, 0.0);
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
    const hog::DVec2 overhead = drawn_at(shot, feet + (up * 2.5));
    const Rgb above = pixel(*alone, overhead);
    const Rgb still_above = pixel(*seen, overhead);
    INFO("before " << before.r << ", " << before.g << ", " << before.b);
    INFO("after " << after.r << ", " << after.g << ", " << after.b);
    // Hiding the ground behind, redder than it though seen against the sun, whatever grows
    // there; and nothing over their head.
    CHECK(std::abs(after.r - before.r) + std::abs(after.g - before.g) +
              std::abs(after.b - before.b) >
          60);
    CHECK(after.r - after.g > before.r - before.g);
    CHECK(still_above.r == above.r);
    CHECK(still_above.g == above.g);
    CHECK(still_above.b == above.b);
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
    const Rgb wet = reddest(*seen, under);
    const Rgb dry = reddest(*seen, drawn_at(shot, surface + (up * 0.5)));
    INFO("water " << water.r << ", " << water.g << ", " << water.b);
    INFO("wet " << wet.r << ", " << wet.g << ", " << wet.b);
    INFO("dry " << dry.r << ", " << dry.g << ", " << dry.b);
    // Not hidden by the surface, but dimmed by the water above.
    CHECK(wet.r - wet.g > water.r - water.g + 15);
    CHECK(wet.r < dry.r - 10);
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
    REQUIRE(hog::client::draw_still(*device, *scene, over_the_woods(), extent));
    std::vector<std::string_view> names;
    for (const hog::render::GpuSpan& span : scene->timings()) {
        names.push_back(span.name);
        CHECK(span.milliseconds >= 0.0);
    }
    // From this high no tree is near enough to grow, so the forest pass isn't added at all.
    CHECK(names == std::vector<std::string_view>{"uploads", "sky tables", "sky", "foam", "ripples",
                                                 "sea laid", "ground", "trees", "behind sea",
                                                 "behind sea depth", "sea", "rivers", "glare"});
    // Over the woods, the stamps draw trees; from this high, none are grown.
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
    CHECK(!hog::client::draw_still(*device, *scene, over_the_woods(), extent, 1));
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
    // A sea grown under a fresh breeze, crossed by a swell from a storm far off; and one under a
    // gale.
    const hog::protocol::CellWaves swell{.height = 1.4F, .peak = 0.45F, .heading = {-0.3F, 0.95F}};
    const hog::client::Forecast fresh =
        everywhere({.drift = {16.0F, 6.0F},
                    .wind = {9.4F, 3.5F},
                    .wind_sea = {.height = 2.2F, .peak = 0.75F, .heading = {0.94F, 0.35F}},
                    .swells = {swell, hog::protocol::CellWaves{}},
                    .current = {}});
    const hog::client::Forecast gale =
        everywhere({.drift = {30.0F, 11.0F},
                    .wind = {17.0F, 6.3F},
                    .wind_sea = {.height = 5.5F, .peak = 0.52F, .heading = {0.94F, 0.35F}},
                    .swells = {swell, hog::protocol::CellWaves{}},
                    .current = {}});
    struct Look {
        std::string_view name;
        double pitch;
        double time_of_day;
    };
    for (const Look& look : {Look{.name = "down", .pitch = -0.6, .time_of_day = 0.5},
                             Look{.name = "out", .pitch = -0.12, .time_of_day = 0.5},
                             Look{.name = "level", .pitch = 0.0, .time_of_day = 0.5},
                             Look{.name = "evening", .pitch = -0.05, .time_of_day = 0.64},
                             Look{.name = "shore", .pitch = -0.1, .time_of_day = 0.5},
                             Look{.name = "beach", .pitch = -0.08, .time_of_day = 0.45},
                             Look{.name = "coast", .pitch = -0.25, .time_of_day = 0.45},
                             Look{.name = "t30", .pitch = -0.1, .time_of_day = 0.30},
                             Look{.name = "t38", .pitch = -0.1, .time_of_day = 0.38},
                             Look{.name = "t62", .pitch = -0.1, .time_of_day = 0.62},
                             Look{.name = "t70", .pitch = -0.1, .time_of_day = 0.70}}) {
        Shot shot = at_sea(look.pitch);
        if (look.name == "shore") {
            shot = over_the_woods(20.0, look.pitch);
        } else if (look.name == "beach") {
            shot = off_the_coast(-3.0, 2.0, false, look.pitch);
        } else if (look.name == "coast") {
            shot = off_the_coast(350.0, 60.0, true, look.pitch);
        }
        const hog::DVec3 up = glm::normalize(shot.camera.eye.metres());
        shot.time =
            hog::client::start_time(calendar(),
                                    hog::to_game_time(hog::WallClock::time_point{
                                        std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}}),
                                    up, look.time_of_day);
        for (const auto& [sea, forecast] : {std::pair{"fresh", &fresh}, std::pair{"gale", &gale}}) {
            shot.forecast = forecast;
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("{}_{}.png", sea, look.name), *still));
        }
    }
}

namespace {

// The middle of the widest lake of the first few found anywhere, and its level.
std::optional<std::pair<hog::DVec3, double>> a_lake() {
    using hog::terrain::shore_size;
    using hog::terrain::shore_spacing;
    const hog::planet::Elevation& terrain = world().geology().terrain();
    const double radius = world().geology().planet_radius();
    std::optional<std::pair<hog::DVec3, double>> widest;
    std::size_t most = 0;
    int found = 0;
    constexpr int points = 400'000;
    for (int i = 0; i < points && found < 6; ++i) {
        const double z = 1.0 - ((2.0 * (i + 0.5)) / points);
        const double around = i * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const double across = std::sqrt(1.0 - (z * z));
        const hog::DVec3 at{across * std::cos(around), across * std::sin(around), z};
        const auto lake = terrain.lake(hog::planet::cell_at(at));
        if (!lake) {
            continue;
        }
        ++found;
        const auto level = static_cast<float>(hog::planet::fixed_to_double(lake->surface));
        const hog::terrain::ShoreDepths shore =
            hog::terrain::work_out_shore(world(), hog::terrain::shore_frame(at, radius), radius);
        // Its texels in the finest layer, and their middle.
        hog::DVec2 sum{0.0};
        std::size_t count = 0;
        const double first = (0.5 - (shore_size / 2.0)) * shore_spacing[0];
        for (std::size_t texel = 0; texel < std::size_t{shore_size} * shore_size; ++texel) {
            if (shore.levels[texel] == level && shore.still[texel] > 0.0F) {
                const std::size_t row = texel / shore_size;
                const std::size_t column = texel % shore_size;
                sum += hog::DVec2{first + (static_cast<double>(column) * shore_spacing[0]),
                                  first + (static_cast<double>(row) * shore_spacing[0])};
                ++count;
            }
        }
        if (count > most) {
            most = count;
            widest = {hog::terrain::shore_direction(shore.frame, radius,
                                                    sum / static_cast<double>(count)),
                      static_cast<double>(level)};
        }
    }
    return widest;
}

} // namespace

// Not a check: stills of a lake above the sea, under a breeze and a gale, to look at. Saved where
// HOG_LOOK names.
TEST_CASE("a lake, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto lake = a_lake();
    REQUIRE(lake);
    const auto [middle, level] = lake.value_or(std::pair{hog::DVec3{0.0, 0.0, 1.0}, 0.0});
    WARN("a lake " << level << " m up, at " << middle.x << ", " << middle.y << ", " << middle.z);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::protocol::CellWaves none{};
    const hog::client::Forecast fresh = everywhere(
        {.drift = {16.0F, 6.0F}, .wind = {9.4F, 3.5F}, .swells = {none, none}, .current = {}});
    const hog::client::Forecast gale = everywhere(
        {.drift = {30.0F, 11.0F}, .wind = {17.0F, 6.3F}, .swells = {none, none}, .current = {}});
    const double radius = world().geology().planet_radius();
    const hog::DVec3 pole{0.0, 0.0, 1.0};
    const hog::DVec3 north = glm::normalize(pole - (middle * glm::dot(middle, pole)));
    // The last from well beyond the depths kept round the eye, to see the lake as the ground has
    // it.
    for (const auto& [name, altitude, pitch, back] :
         {std::tuple{"low", 2.0, -0.08, 0.0}, std::tuple{"high", 60.0, -0.45, 0.0},
          std::tuple{"far", 400.0, -0.35, 0.0}, std::tuple{"distant", 3'000.0, -0.24, 12'000.0}}) {
        Shot shot = looking_north(glm::normalize(middle - (north * (back / radius))),
                                  level + altitude, pitch);
        const hog::client::Forecast calm = everywhere(
            {.drift = {0.0F, 0.0F}, .wind = {0.0F, 0.0F}, .swells = {none, none}, .current = {}});
        for (const auto& [sea, forecast] :
             {std::pair{"fresh", &fresh}, std::pair{"gale", &gale}, std::pair{"calm", &calm}}) {
            shot.forecast = forecast;
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("{}_lake_{}.png", sea, name), *still));
        }
    }
}

// Not a check: stills of people standing in the surf off a beach, seen through the water as high
// as the waves stand round them, under a breeze and a gale. Saved where HOG_LOOK names.
TEST_CASE("wading in the surf, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::protocol::CellWaves swell{.height = 1.4F, .peak = 0.45F, .heading = {-0.3F, 0.95F}};
    const hog::client::Forecast fresh =
        everywhere({.drift = {16.0F, 6.0F},
                    .wind = {9.4F, 3.5F},
                    .wind_sea = {.height = 2.2F, .peak = 0.75F, .heading = {0.94F, 0.35F}},
                    .swells = {swell, hog::protocol::CellWaves{}},
                    .current = {}});
    const hog::client::Forecast gale =
        everywhere({.drift = {30.0F, 11.0F},
                    .wind = {17.0F, 6.3F},
                    .wind_sea = {.height = 5.5F, .peak = 0.52F, .heading = {0.94F, 0.35F}},
                    .swells = {swell, hog::protocol::CellWaves{}},
                    .current = {}});
    Shot shot = off_the_coast(-3.0, 2.0, false, -0.12);
    const double radius = world().geology().planet_radius();
    // Each where the ground under them lies, from a little way out to further, a little apart.
    const hog::DVec3 up = glm::normalize(shot.camera.eye.metres());
    const hog::DVec3 aside = glm::normalize(glm::cross(shot.camera.forward, up));
    for (const auto& [along, across] : {std::pair{12.0, -2.0}, std::pair{20.0, 2.0},
                                        std::pair{32.0, -4.0}, std::pair{50.0, 4.0}}) {
        const hog::DVec3 there = glm::normalize((ahead(shot, along) + (aside * across)).metres());
        const double ground =
            hog::client::ground_under(world(), hog::WorldPosition{there * radius});
        shot.others.push_back({.feet = hog::WorldPosition{there * (radius + ground)},
                               .up = there,
                               .colour = {0.6F, 0.1F, 0.05F}});
    }
    for (const auto& [sea, forecast] : {std::pair{"fresh", &fresh}, std::pair{"gale", &gale}}) {
        shot.forecast = forecast;
        for (int moment = 0; moment < 3; ++moment) {
            shot.time = shot.time + std::chrono::seconds{3};
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(std::filesystem::path{where} /
                                                  std::format("{}_wading_{}.png", sea, moment),
                                              *still));
        }
    }
}

// Not a check: stills from under the sea and at its surface, someone standing out of it ahead, to
// look at. Saved where HOG_LOOK names.
TEST_CASE("under the sea, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::client::Forecast fresh =
        everywhere({.drift = {16.0F, 6.0F},
                    .wind = {9.4F, 3.5F},
                    .wind_sea = {.height = 1.2F, .peak = 0.75F, .heading = {0.94F, 0.35F}},
                    .swells = {},
                    .current = {}});
    const double radius = world().geology().planet_radius();
    // Over four metres of water.
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"waterline", -1.0, 0.0}, std::tuple{"shallow", -2.5, -0.1},
          std::tuple{"up", -3.0, 0.7}, std::tuple{"bed", -3.5, -0.3}}) {
        Shot shot = off_the_coast(120.0, altitude, true, pitch);
        shot.forecast = &fresh;
        const hog::DVec3 there = glm::normalize(ahead(shot, 8.0).metres());
        const double ground =
            hog::client::ground_under(world(), hog::WorldPosition{there * radius});
        shot.others = {{.feet = hog::WorldPosition{there * (radius + std::max(ground, -1.0))},
                        .up = there,
                        .colour = {0.6F, 0.1F, 0.05F}}};
        for (int moment = 0; moment < 3; ++moment) {
            shot.time = shot.time + std::chrono::milliseconds{700};
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(std::filesystem::path{where} /
                                                  std::format("under_{}_{}.png", name, moment),
                                              *still));
        }
    }
    // On a calm sea, by steps of a few centimetres, so the near plane straddles its surface in one,
    // wherever the tide stands.
    for (int step = 0; step < 12; ++step) {
        Shot shot = off_the_coast(120.0, -0.55 + (0.1 * step), true, 0.0);
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(std::filesystem::path{where} /
                                              std::format("under_straddling_{:02}.png", step),
                                          *still));
    }
}

// Not a check: stills of still water cut off from the sea, under a breeze and a gale, to look at.
// Saved where HOG_LOOK names.
TEST_CASE("still water, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::protocol::CellWaves none{};
    const hog::client::Forecast fresh = everywhere(
        {.drift = {16.0F, 6.0F}, .wind = {9.4F, 3.5F}, .swells = {none, none}, .current = {}});
    const hog::client::Forecast gale = everywhere(
        {.drift = {30.0F, 11.0F}, .wind = {17.0F, 6.3F}, .swells = {none, none}, .current = {}});
    const std::optional<hog::DVec3> under = still_water();
    REQUIRE(under);
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"low", 2.0, -0.08}, std::tuple{"high", 60.0, -0.45}}) {
        Shot shot = looking_north(under.value_or(hog::DVec3{0.0, 0.0, 1.0}), altitude, pitch);
        for (const auto& [sea, forecast] : {std::pair{"fresh", &fresh}, std::pair{"gale", &gale}}) {
            shot.forecast = forecast;
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("{}_still_{}.png", sea, name), *still));
        }
    }
}

// Not a check: stills of people walking and running through the shallows of a calm sea, the wakes
// they leave seen from beside them and from over them. Saved where HOG_LOOK names.
TEST_CASE("wakes, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::client::Forecast calm =
        everywhere({.drift = {3.0F, 1.0F},
                    .wind = {2.5F, 0.8F},
                    .wind_sea = {.height = 0.15F, .peak = 2.0F, .heading = {0.94F, 0.35F}},
                    .swells = {hog::protocol::CellWaves{}, hog::protocol::CellWaves{}},
                    .current = {}});
    const double radius = world().geology().planet_radius();
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"beside", 2.0, -0.12}, std::tuple{"over", 8.0, -0.6}}) {
        Shot shot = off_the_coast(-3.0, altitude, false, pitch);
        shot.forecast = &calm;
        const hog::DVec3 up = glm::normalize(shot.camera.eye.metres());
        const hog::DVec3 aside = glm::normalize(glm::cross(shot.camera.forward, up));
        // Each from `along` metres ahead, `across` aside, at so many m/s across the view.
        const auto walking = [&](double along, double across, double speed, double seconds) {
            const hog::DVec3 there = glm::normalize(
                (ahead(shot, along) + (aside * (across + (speed * seconds)))).metres());
            const double ground =
                hog::client::ground_under(world(), hog::WorldPosition{there * radius});
            return hog::render::Capsule{.feet = hog::WorldPosition{there * (radius + ground)},
                                        .up = there,
                                        .colour = {0.6F, 0.1F, 0.05F}};
        };
        const auto stood = [&](double seconds) {
            return std::vector{walking(14.0, -8.0, 1.4, seconds), walking(20.0, 6.0, -1.4, seconds),
                               walking(26.0, -14.0, 3.5, seconds)};
        };
        shot.others = stood(0.0);
        REQUIRE(hog::client::draw_still(*device, *scene, shot, {.width = 96, .height = 54}));
        constexpr int frames = 180;
        for (int frame = 1; frame <= frames; ++frame) {
            shot.time = shot.time + std::chrono::microseconds{33'333};
            shot.others = stood(frame / 30.0);
            const auto still =
                hog::client::draw_still(*device, *scene, shot,
                                        frame < frames ? VkExtent2D{.width = 96, .height = 54}
                                                       : VkExtent2D{.width = 960, .height = 540});
            REQUIRE(still);
            if (frame == frames) {
                REQUIRE(hog::client::save_picture(
                    std::filesystem::path{where} / std::format("calm_wakes_{}.png", name), *still));
            }
        }
    }
}

// Not a check: stills of rain falling on a calm sea, lighter and heavier, seen from standing in the
// shallows and from over them. Saved where HOG_LOOK names.
TEST_CASE("rain on the sea, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto raining = [](float falling) {
        return everywhere({.cloud = 0.8F,
                           .falling = falling,
                           .drift = {2.0F, 0.5F},
                           .wind = {1.5F, 0.4F},
                           .base = 1'200.0F,
                           .top = 3'000.0F,
                           .swells = {hog::protocol::CellWaves{}, hog::protocol::CellWaves{}},
                           .current = {}});
    };
    const hog::client::Forecast light = raining(2.0F);
    const hog::client::Forecast heavy = raining(12.0F);
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"standing", 1.7, -0.45}, std::tuple{"over", 6.0, -0.5}}) {
        Shot shot = off_the_coast(-3.0, altitude, false, pitch);
        for (const auto& [rain, forecast] :
             {std::pair{"light", &light}, std::pair{"heavy", &heavy}}) {
            shot.forecast = forecast;
            const auto still =
                hog::client::draw_still(*device, *scene, shot, {.width = 960, .height = 540});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("{}_rain_{}.png", rain, name), *still));
        }
    }
}

TEST_CASE("a windy meadow, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const hog::protocol::CellWaves none{};
    const hog::client::Forecast fresh = everywhere(
        {.drift = {16.0F, 6.0F}, .wind = {9.4F, 3.5F}, .swells = {none, none}, .current = {}});
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"high", 120.0, -0.5}, std::tuple{"above", 60.0, -1.0},
          std::tuple{"low", 20.0, -0.35}}) {
        Shot shot = on_the_island(altitude, pitch);
        shot.forecast = &fresh;
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("meadow_{}.png", name), *still));
    }
}

TEST_CASE("mountains, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto& terrain = world().geology().terrain();
    const double radius = world().geology().planet_radius();
    const auto belt =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.mountain_strength(cell) > hog::planet::fixed_one * 7 / 10 &&
                   terrain.elevation(cell) > 1'200 * hog::planet::fixed_one;
        });
    const auto cone =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.continents().height(cell) > 0 &&
                   terrain.volcanoes().lift(cell) > 800 * hog::planet::fixed_one;
        });
    REQUIRE(belt);
    const hog::planet::CellId ridge = belt.value_or(hog::planet::CellId{});
    for (const auto& [name, place, altitude, pitch, toward] :
         {std::tuple{"belt_high", ridge, 3'500.0, -0.3, hog::planet::north_pole},
          std::tuple{"belt_low", ridge, 2'000.0, -0.1, hog::planet::north_pole},
          std::tuple{"cone", cone.value_or(ridge), 2'500.0, -0.2, hog::planet::north_pole}}) {
        const hog::DVec3 at = hog::planet::cell_center(place);
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
        // Back off a few kilometres to see it whole.
        const hog::DVec3 back = glm::normalize(at - (frame.north * (4'000.0 / radius)));
        hog::client::Flight flight = hog::client::start_flight(back, radius, altitude, toward);
        flight.pitch = pitch;
        const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                   {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("mountains_{}.png", name), *still));
    }
}

TEST_CASE("an evolved world, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" /
                                                  "planet.toml")
                      .value();
    config.generator = hog::planet::Generator::Evolved;
    const hog::planet::World evolved{config};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, evolved, jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto& terrain = evolved.geology().terrain();
    const double radius = evolved.geology().planet_radius();
    const auto range =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.mountain_strength(cell) > hog::planet::fixed_one / 2 &&
                   terrain.elevation(cell) > 2'000 * hog::planet::fixed_one;
        });
    const auto plain =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.mountain_strength(cell) < hog::planet::fixed_one / 10 &&
                   terrain.elevation(cell) > 100 * hog::planet::fixed_one &&
                   terrain.elevation(cell) < 800 * hog::planet::fixed_one;
        });
    const auto coast =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.is_land(cell) && terrain.elevation(cell) < 30 * hog::planet::fixed_one;
        });
    REQUIRE(range);
    REQUIRE(plain);
    REQUIRE(coast);
    const hog::planet::CellId high = range.value_or(hog::planet::CellId{});
    for (const auto& [name, place, back_by, altitude, pitch] :
         {std::tuple{"range_high", high, 12'000.0, 6'000.0, -0.3},
          std::tuple{"range_low", high, 6'000.0, 3'000.0, -0.1},
          std::tuple{"plain", plain.value_or(hog::planet::CellId{}), 3'000.0, 600.0, -0.15},
          std::tuple{"coast", coast.value_or(hog::planet::CellId{}), 4'000.0, 800.0, -0.2},
          std::tuple{"overview", high, 40'000.0, 25'000.0, -0.5}}) {
        const hog::DVec3 at = hog::planet::cell_center(place);
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
        const hog::DVec3 back = glm::normalize(at - (frame.north * (back_by / radius)));
        const double under =
            std::max({hog::client::ground_under(evolved, hog::WorldPosition{back * radius}),
                      hog::planet::fixed_to_double(terrain.elevation(place)), 0.0});
        hog::client::Flight flight = hog::client::start_flight(back, radius, under + altitude, at);
        flight.pitch = pitch;
        const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                   {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("evolved_{}.png", name), *still));
    }
}

TEST_CASE("an evolved world as blocks, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" /
                                                  "planet.toml")
                      .value();
    config.generator = hog::planet::Generator::Evolved;
    const hog::planet::World evolved{config};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, evolved, jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    scene->set_blocks(true);
    const auto& terrain = evolved.geology().terrain();
    const double radius = evolved.geology().planet_radius();
    const auto range =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.mountain_strength(cell) > hog::planet::fixed_one / 2 &&
                   terrain.elevation(cell) > 1'500 * hog::planet::fixed_one;
        });
    const auto coast =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.is_land(cell) && terrain.elevation(cell) < 30 * hog::planet::fixed_one;
        });
    REQUIRE(range);
    REQUIRE(coast);
    for (const auto& [name, place, back_by, altitude, pitch] :
         {std::tuple{"range", range.value_or(hog::planet::CellId{}), 1'500.0, 400.0, -0.3},
          std::tuple{"coast", coast.value_or(hog::planet::CellId{}), 800.0, 150.0, -0.25},
          std::tuple{"coast_far", coast.value_or(hog::planet::CellId{}), 2'500.0, 1'200.0,
                     -0.45}}) {
        const hog::DVec3 at = hog::planet::cell_center(place);
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
        const hog::DVec3 back = glm::normalize(at - (frame.north * (back_by / radius)));
        const double under =
            std::max({hog::client::ground_under(evolved, hog::WorldPosition{back * radius}),
                      hog::planet::fixed_to_double(terrain.elevation(place)), 0.0});
        hog::client::Flight flight = hog::client::start_flight(back, radius, under + altitude, at);
        flight.pitch = pitch;
        const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                   {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("blocks_{}.png", name), *still));
    }
}

TEST_CASE("the globe's voxels, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World world{config};
    const double radius = world.geology().planet_radius();
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    for (const char* name : {"earth", "moon"}) {
        const hog::client::GlobeWorld globe{
            hog::client::globe_planet(name, config.seed, radius).value()};
        auto scene = Scene::create(*device, world, jobs, calendar(), hog::client::still_format, 1);
        REQUIRE(scene);
        scene->set_blocks(true);
        scene->follow_globe(globe.world);
        const hog::DVec3 noon = glm::normalize(hog::DVec3{1.0, 0.2, 0.3});
        const hog::DVec3 at = glm::normalize(noon + hog::DVec3{0.0, 0.0, 0.3});
        const double ground = hog::client::globe_ground(globe.generator, noon);
        for (const auto& [shot, height, pitch] :
             {std::tuple{"high", 60.0, -0.6}, std::tuple{"low", 6.0, -0.12}}) {
            hog::client::Flight flight =
                hog::client::start_flight(noon, radius, ground + height, at);
            flight.pitch = pitch;
            const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                       {.width = 1600, .height = 900});
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
    const double radius = world().geology().planet_radius();
    const hog::client::GlobeWorld globe{
        hog::client::globe_planet(std::nullopt, hog::client::testing::planet().seed, radius)
            .value()};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    scene->set_blocks(true);
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
                                  .pitch = -9.7}}) {
        const hog::DVec3 up =
            hog::planet::direction_at(spot.latitude * degree, spot.longitude * degree);
        const hog::client::Flight flight = hog::client::turned_to(
            hog::client::start_flight(up, radius, spot.altitude, hog::planet::north_pole),
            spot.heading * degree, spot.pitch * degree);
        const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                   {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("globe_at_{}.png", spot.name), *still));
    }
}

TEST_CASE("fjords, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World fine{config};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, fine, jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto& terrain = fine.geology().terrain();
    const double radius = fine.geology().planet_radius();
    const auto fjord =
        hog::planet::nearest_cell(hog::planet::start_direction, 9, [&](hog::planet::CellId cell) {
            return terrain.continents().height(cell) > hog::planet::fixed_one / 20 &&
                   !terrain.is_land(cell);
        });
    REQUIRE(fjord);
    const hog::DVec3 at = hog::planet::cell_center(fjord.value_or(hog::planet::CellId{}));
    // The deepest trough the ice left inland, high above the sea.
    const auto& landform = terrain.landform();
    std::uint32_t inland = 0;
    for (std::uint32_t index = 0; index < landform.grid().size(); ++index) {
        if (landform.heights()[index] > 300 * hog::planet::fixed_one &&
            landform.carved()[index] > landform.carved()[inland]) {
            inland = index;
        }
    }
    {
        const hog::DVec3 valley = hog::planet::cell_center(landform.grid().cell(inland));
        const hog::planet::TangentFrame across = hog::planet::tangent_frame(valley);
        int view = 0;
        for (const auto& [away, altitude, pitch] :
             {std::tuple{across.north, 3'000.0, -0.35}, std::tuple{across.east, 3'000.0, -0.35},
              std::tuple{-across.north, 3'000.0, -0.35}}) {
            const hog::DVec3 back = glm::normalize(valley + (away * (6'000.0 / radius)));
            hog::client::Flight flight = hog::client::start_flight(
                back, radius, hog::planet::fixed_to_double(landform.heights()[inland]) + altitude,
                valley);
            flight.pitch = pitch;
            const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                       {.width = 1600, .height = 900});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("inland_{}.png", view++), *still));
        }
    }
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(at);
    int shot = 0;
    for (const auto& [away, altitude, pitch] :
         {std::tuple{frame.north, 1'800.0, -0.3}, std::tuple{frame.east, 1'800.0, -0.3},
          std::tuple{-frame.north, 1'800.0, -0.3}, std::tuple{-frame.east, 1'800.0, -0.3},
          std::tuple{frame.north, 6'000.0, -0.7}, std::tuple{-frame.east, 300.0, -0.05}}) {
        const hog::DVec3 back = glm::normalize(at + (away * (5'000.0 / radius)));
        hog::client::Flight flight = hog::client::start_flight(back, radius, altitude, at);
        flight.pitch = pitch;
        const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                   {.width = 1600, .height = 900});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("fjord_{}.png", shot++), *still));
    }
}

TEST_CASE("rivers, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World fine{config};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, fine, jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto& terrain = fine.geology().terrain();
    const double radius = fine.geology().planet_radius();
    const auto reaches = terrain.rivers().reaches();
    REQUIRE(!reaches.empty());
    // The widest river near the start, and a stream.
    const auto pick = [&](double narrowest, double widest) {
        const hog::planet::Reach* best = nullptr;
        double nearest = 1e9;
        for (const auto& reach : reaches) {
            const double away = glm::length(reach.from - hog::planet::start_direction) * radius;
            if (reach.width >= narrowest && reach.width <= widest && reach.fall == 0 &&
                reach.to_surface > 40 * hog::planet::fixed_one && away < nearest) {
                nearest = away;
                best = &reach;
            }
        }
        return best;
    };
    for (const auto& [kind, narrowest, widest] :
         {std::tuple{"river", 15.0, 1e9}, std::tuple{"stream", 4.0, 10.0}}) {
        const hog::planet::Reach* reach = pick(narrowest, widest);
        if (reach == nullptr) {
            continue;
        }
        const hog::DVec3 at =
            terrain.rivers().meander().unshifted(hog::planet::course_at(*reach, 0.5));
        const double ground = hog::planet::fixed_to_double(
            terrain.elevation(hog::planet::cell_at(at, hog::planet::max_level)));
        const hog::DVec3 along = glm::normalize(
            terrain.rivers().meander().unshifted(hog::planet::course_at(*reach, 0.6)) -
            terrain.rivers().meander().unshifted(hog::planet::course_at(*reach, 0.4)));
        const hog::DVec3 side = glm::normalize(glm::cross(at, along));
        int shot = 0;
        for (const auto& [away, back, altitude, pitch] :
             {std::tuple{side, 25.0, 3.0, -0.2}, std::tuple{side, 250.0, 25.0, -0.15},
              std::tuple{-along, 400.0, 60.0, -0.2}, std::tuple{side, 1'500.0, 500.0, -0.35},
              std::tuple{-along, 3'000.0, 1'500.0, -0.45}, std::tuple{side, 10.0, 1'200.0, -1.5},
              std::tuple{-side, 7'000.0, 1'500.0, -0.22},
              std::tuple{-side, 14'000.0, 3'000.0, -0.22}}) {
            const hog::DVec3 from = glm::normalize(at + (away * (back / radius)));
            hog::client::Flight flight =
                hog::client::start_flight(from, radius, ground + altitude, at);
            flight.pitch = pitch;
            const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                       {.width = 1600, .height = 900});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("{}_{}.png", kind, shot++), *still));
        }
    }
}

TEST_CASE("river mouths, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World fine{config};
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, fine, jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto& terrain = fine.geology().terrain();
    const double radius = fine.geology().planet_radius();
    const auto reaches = terrain.rivers().reaches();
    int shot = 0;
    for (const int kind : {0, 1, 2}) {
        const bool sea = kind == 0;
        const hog::planet::Reach* best = nullptr;
        double nearest = 1e9;
        for (const auto& reach : reaches) {
            const double away = glm::length(reach.from - hog::planet::start_direction) * radius;
            const bool wanted =
                kind == 2 ? reach.outflow : reach.mouth && (reach.to_surface == 0) == sea;
            if (wanted && reach.width >= 8.0 && away < nearest) {
                nearest = away;
                best = &reach;
            }
        }
        if (best == nullptr) {
            continue;
        }
        const auto& meander = terrain.rivers().meander();
        const hog::DVec3 at =
            meander.unshifted(hog::planet::course_at(*best, kind == 2 ? 0.3 : 0.7));
        const double level =
            hog::planet::fixed_to_double(kind == 2 ? best->from_surface : best->to_surface);
        const hog::DVec3 along =
            glm::normalize(meander.unshifted(hog::planet::course_at(*best, 0.9)) -
                           meander.unshifted(hog::planet::course_at(*best, 0.5)));
        const hog::DVec3 side = glm::normalize(glm::cross(at, along));
        for (const auto& [away, back, altitude, pitch] :
             {std::tuple{side, 120.0, 25.0, -0.2}, std::tuple{-along, 300.0, 60.0, -0.2},
              std::tuple{side, 600.0, 250.0, -0.45}, std::tuple{-along, 40.0, 400.0, -1.4}}) {
            const hog::DVec3 from = glm::normalize(at + (away * (back / radius)));
            hog::client::Flight flight =
                hog::client::start_flight(from, radius, level + altitude, at);
            flight.pitch = pitch;
            const auto still = hog::client::draw_still(*device, *scene, shot_of(flight),
                                                       {.width = 1600, .height = 900});
            REQUIRE(still);
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} / std::format("mouth_{}.png", shot++), *still));
        }
    }
}

// Not a check: stills of the spots lakes looked wrong at, from the world as the client has it.
// Saved where HOG_LOOK names.
TEST_CASE("lakes at reported spots, to look at", "[.look]") {
    using namespace hog;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const planet::World full{
        planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} / "config" / "planet.toml")
            .value()};
    const auto device = test_device();
    jobs::JobSystem jobs{4};
    auto scene = Scene::create(*device, full, jobs, calendar(), client::still_format, 1);
    REQUIRE(scene);
    const auto spot = [](std::string_view name, double latitude, double longitude, double altitude,
                         double heading, double pitch) {
        return client::FixedView{
            .name = name,
            .place = {.latitude = latitude, .longitude = longitude, .altitude = altitude},
            .heading = heading,
            .pitch = pitch};
    };
    for (const client::FixedView& view : {
             spot("edge", -24.785802, 63.217964, 99.0, 55.4, -15.6),
             spot("edge_se", -24.785802, 63.249, 99.0, 116.6, -15.6),
             spot("edge_nw", -24.785802, 63.249, 99.0, 296.6, -15.6),
             spot("edge_bank", -24.785802, 63.2553, 99.7, 116.6, -15.6),
             spot("square_n", -50.727280, 41.779543, 358.0, 0.0, -25.0),
             spot("square_e", -50.727280, 41.779543, 358.0, 90.0, -25.0),
             spot("square_s", -50.727280, 41.779543, 358.0, 180.0, -25.0),
             spot("square_w", -50.727280, 41.779543, 358.0, 270.0, -25.0),
             spot("seam_n", -49.582354, 42.014867, 373.0, 0.0, -20.0),
             spot("seam_e", -49.582354, 42.014867, 373.0, 90.0, -20.0),
             spot("seam_s", -49.582354, 42.014867, 373.0, 180.0, -20.0),
             spot("seam_w", -49.582354, 42.014867, 373.0, 270.0, -20.0),
             spot("box_under", -68.190071, 179.063260, 1665.0, -133.6, 10.0),
             spot("box_over", -68.190071, 179.063260, 1740.0, -133.6, -15.0),
             spot("lines", -68.467951, -178.644561, 1673.0, 157.0, 62.5),
             spot("walls_n", -67.471556, 177.539819, 1760.0, 0.0, -12.0),
             spot("walls_s", -67.471556, 177.539819, 1760.0, 180.0, -12.0),
             spot("block", -50.727280, 41.779543, 358.0, 120.0, -45.0),
             spot("block_s", -50.727280, 41.779543, 358.0, 180.0, -45.0),
             spot("block_grid36", -42.568842, 40.193497, 2231.0, 112.1, -28.3),
             spot("block_cast", -42.866120, 42.857210, 415.0, 113.1, -23.6),
             spot("block_cast2", -42.746470, 42.477666, 682.0, 113.4, -23.6),
             spot("block_corners37", -43.771076, 39.591338, 290.0, 87.7, -59.6),
             spot("block_high", -42.568842, 40.193497, 3500.0, 112.1, -6.0),
             spot("under37", -43.771076, 39.591338, 238.4, 87.7, 45.0),
             spot("under37_w", -43.771076, 39.591338, 238.4, 267.7, 45.0),
             spot("under37b", -43.771076, 39.594, 238.4, 87.7, 45.0),
             spot("under37b_w", -43.771076, 39.594, 238.4, 267.7, 45.0),
             spot("over37b", -43.771076, 39.594, 250.0, 87.7, -30.0),
             spot("block_high2", -42.568842, 40.193497, 3500.0, 292.1, -6.0),
             spot("shoal", -28.107171, -44.351725, 21.0, -24.0, -29.9),
             spot("shoal_low", -28.107171, -44.351725, 4.0, -24.0, -8.0),
         }) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
        if (const char* only = std::getenv("HOG_LOOK_ONLY");
            only != nullptr && !view.name.starts_with(only)) {
            continue;
        }
        const hog::protocol::CellWaves swell{
            .height = 1.4F, .peak = 0.12F, .heading = {0.25F, -0.97F}};
        const hog::client::Forecast fresh =
            everywhere({.drift = {16.0F, 6.0F},
                        .wind = {6.0F, 2.0F},
                        .wind_sea = {.height = 0.8F, .peak = 0.4F, .heading = {0.94F, 0.35F}},
                        .swells = {swell, hog::protocol::CellWaves{}},
                        .current = {}});
        Shot shot = client::shot_of(view, full, calendar());
        if (view.name.starts_with("shoal")) {
            shot.forecast = &fresh;
        }
        const int moments = view.name.starts_with("shoal") ? 3 : 1;
        for (int moment = 0; moment < moments; ++moment) {
            const auto still = client::draw_still(
                *device, *scene, shot,
                (view.name.starts_with("block") || view.name.starts_with("shoal"))
                    ? VkExtent2D{.width = 1280, .height = 720}
                    : VkExtent2D{.width = 640, .height = 360});
            REQUIRE(still);
            REQUIRE(client::save_picture(std::filesystem::path{where} /
                                             (moment == 0
                                                  ? std::format("{}.png", view.name)
                                                  : std::format("{}_{}.png", view.name, moment)),
                                         *still));
            shot.time = shot.time + std::chrono::seconds{2};
        }
    }
}

TEST_CASE("broken cloud from near and far, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    const hog::client::Forecast broken =
        everywhere({.cloud = 0.1F, .humidity = 0.6F, .base = 1'500.0F, .top = 3'500.0F});
    const hog::DVec3 under = glm::normalize(hog::DVec3{0.3, -0.9, 0.3});
    for (const auto& [name, altitude, pitch] :
         {std::tuple{"down9k", 9'000.0, -1.55}, std::tuple{"down5k", 5'000.0, -1.55},
          std::tuple{"down9k_again", 9'000.0, -1.55}, std::tuple{"side4k", 4'000.0, -0.3},
          std::tuple{"side9k", 9'000.0, -0.3}}) {
        auto scene =
            Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
        REQUIRE(scene);
        Shot shot = looking_north(under, altitude, pitch);
        shot.forecast = &broken;
        const auto still =
            hog::client::draw_still(*device, *scene, shot, {.width = 1200, .height = 800});
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("clouds_{}.png", name), *still));
    }
}

TEST_CASE("broken cloud while descending, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto device = test_device();
    hog::jobs::JobSystem jobs{4};
    const hog::client::Forecast broken =
        everywhere({.cloud = 0.1F, .humidity = 0.6F, .base = 1'500.0F, .top = 3'500.0F});
    const hog::DVec3 under = glm::normalize(hog::DVec3{0.3, -0.9, 0.3});
    constexpr VkExtent2D large{.width = 1200, .height = 800};
    auto scene = Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    const auto shot_at = [&](double altitude) {
        Shot shot = looking_north(under, altitude, -0.3);
        shot.forecast = &broken;
        shot.seconds = 0.05;
        return shot;
    };
    REQUIRE(hog::client::draw_still(*device, *scene, shot_at(9'000.0), large));
    auto target = hog::render::make_image(
        *device, {.format = hog::client::still_format,
                  .extent = large,
                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    const auto save = [&](const std::string& name) {
        auto pixels =
            hog::render::read_pixels(*device, *target, hog::render::image_use::colour_target);
        REQUIRE(pixels);
        const auto bytes = std::as_bytes(std::span{*pixels});
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} / std::format("descend_{}.png", name),
            hog::client::Picture{.width = large.width,
                                 .height = large.height,
                                 .texels = {bytes.begin(), bytes.end()}}));
    };
    double altitude = 9'000.0;
    for (int at = 0; at <= 120; ++at) {
        const Shot shot = shot_at(altitude);
        hog::Result<> recorded;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            hog::render::Frame frame{.commands = commands,
                                     .target = {target->handle(), VK_IMAGE_ASPECT_COLOR_BIT},
                                     .view = target->view(),
                                     .format = hog::client::still_format,
                                     .extent = large,
                                     .slot = 0,
                                     .image = 0,
                                     .copyable = true};
            recorded = scene->record(frame, shot);
        }));
        REQUIRE(recorded);
        scene->finish_bakes();
        if (at % 40 == 0) {
            save(std::format("moving_{}", static_cast<int>(altitude)));
        }
        altitude -= 50.0;
    }
    scene = std::unexpected(hog::Error{});
    for (const double settled : {9'000.0, 7'000.0, 5'000.0, 3'000.0}) {
        auto fresh =
            Scene::create(*device, world(), jobs, calendar(), hog::client::still_format, 1);
        REQUIRE(fresh);
        const auto still = hog::client::draw_still(*device, *fresh, shot_at(settled), large);
        REQUIRE(still);
        REQUIRE(hog::client::save_picture(
            std::filesystem::path{where} /
                std::format("descend_still_{}.png", static_cast<int>(settled)),
            *still));
    }
}

TEST_CASE("profile rivers' water laid out", "[.profile]") {
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World fine{config};
    const double radius = fine.geology().planet_radius();
    hog::DVec3 around = hog::planet::start_direction;
    double nearest = 1e9;
    for (const auto& reach : fine.geology().terrain().rivers().reaches()) {
        const double away = glm::length(reach.from - hog::planet::start_direction);
        if (reach.width >= 15.0 && reach.to_surface > 40 * hog::planet::fixed_one &&
            away < nearest) {
            nearest = away;
            around = reach.from;
        }
    }
    for (int run = 0; run < 3; ++run) {
        const auto began = std::chrono::steady_clock::now();
        const auto water = hog::terrain::river_water(fine, around, radius);
        const std::chrono::duration<double, std::milli> took =
            std::chrono::steady_clock::now() - began;
        WARN(std::format("river water: {} vertices, {} triangles, {:.1f} ms", water.vertices.size(),
                         water.indices.size() / 3, took.count()));
    }
}
