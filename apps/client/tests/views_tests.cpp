#include "test_planet.hpp"

#include <hog/client/likeness.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/views.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <test_device.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numbers>
#include <set>
#include <string_view>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::client::FixedView;
using hog::client::Picture;
using hog::client::testing::calendar;
using hog::client::testing::world;
using hog::render::testing::test_device;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkExtent2D reference_extent{.width = 320, .height = 180};

const FixedView& view_named(std::string_view name) {
    for (const FixedView& view : hog::client::fixed_views()) {
        if (view.name == name) {
            return view;
        }
    }
    FAIL("no view named " << name);
    return hog::client::fixed_views().front();
}

Picture still_of(hog::render::Device& device, const FixedView& view, VkExtent2D size = extent) {
    hog::jobs::JobSystem jobs{2};
    auto scene =
        hog::client::Scene::create(device, world(), jobs, calendar(), hog::client::still_format, 1);
    REQUIRE(scene);
    auto still = hog::client::draw_still(device, *scene,
                                         hog::client::shot_of(view, world(), calendar()), size);
    REQUIRE(still);
    return std::move(still).value();
}

double brightness(const Picture& picture) {
    double sum = 0.0;
    for (std::size_t at = 0; at < picture.texels.size(); at += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum += std::to_integer<int>(picture.texels[at + channel]);
        }
    }
    return sum / (0.75 * static_cast<double>(picture.texels.size()));
}

} // namespace

TEST_CASE("each fixed view has a name of its own", "[client][views]") {
    std::set<std::string_view> names;
    for (const FixedView& view : hog::client::fixed_views()) {
        CHECK(!view.name.empty());
        CHECK(names.insert(view.name).second);
    }
    CHECK(names.size() >= 4);
}

TEST_CASE("a fixed view's shot is from its place, facing its way, at its time of day",
          "[client][views]") {
    const double radius = world().geology().planet_radius();
    for (const FixedView& view : hog::client::fixed_views()) {
        INFO(view.name);
        const hog::client::Shot shot = hog::client::shot_of(view, world(), calendar());
        const DVec3 up =
            hog::planet::direction_at(view.place.latitude * degree, view.place.longitude * degree);
        CHECK_THAT(glm::length(shot.camera.eye.metres()) - radius,
                   WithinAbs(view.place.altitude, 1e-3));
        CHECK_THAT(glm::dot(glm::normalize(shot.camera.eye.metres()), up), WithinAbs(1.0, 1e-9));
        CHECK_THAT(std::asin(glm::dot(shot.camera.forward, up)),
                   WithinAbs(view.pitch * degree, 1e-9));
        const DVec3 local = hog::planet::tangent_frame(up).to_local(shot.camera.forward);
        const double heading = std::atan2(local.x, local.y);
        CHECK_THAT(std::remainder(heading - (view.heading * degree), 2.0 * std::numbers::pi),
                   WithinAbs(0.0, 1e-9));
        CHECK_THAT(
            std::remainder(calendar().local_time_of_day(shot.time, up) - view.time_of_day, 1.0),
            WithinAbs(0.0, 1e-6));
        CHECK(!shot.companion);
    }
}

TEST_CASE("the island views stand off its shore, facing it", "[client][views]") {
    const double radius = world().geology().planet_radius();
    for (const std::string_view name : {"island-noon", "shore-morning", "sun-morning"}) {
        INFO(name);
        const FixedView& view = view_named(name);
        const DVec3 up =
            hog::planet::direction_at(view.place.latitude * degree, view.place.longitude * degree);
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(up);
        const DVec3 ahead = (frame.north * std::cos(view.heading * degree)) +
                            (frame.east * std::sin(view.heading * degree));
        const auto ground = [&](double along) {
            return hog::client::ground_under(
                world(),
                hog::WorldPosition{glm::normalize(up + (ahead * (along / radius))) * radius});
        };
        CHECK(ground(0.0) < 0.0);
        bool land = false;
        for (int step = 0; step < 120 && !land; ++step) {
            land = ground(50.0 * step) > 0.0;
        }
        CHECK(land);
    }
}

TEST_CASE("a fixed view is drawn the same each time", "[client][views][gpu]") {
    const auto device = test_device();
    const Picture first = still_of(*device, view_named("island-noon"));
    const Picture again = still_of(*device, view_named("island-noon"));
    const auto difference = hog::client::difference(first, again);
    REQUIRE(difference);
    CHECK(difference->mean == 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the island is darker at night than at noon", "[client][views][gpu]") {
    const auto device = test_device();
    const double noon = brightness(still_of(*device, view_named("island-noon")));
    const double night = brightness(still_of(*device, view_named("island-night")));
    INFO("noon " << noon << ", night " << night);
    CHECK(night < 0.5 * noon);
    CHECK(night > 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("fixed views look as their reference pictures do", "[client][views][gpu][reference]") {
    const auto device = test_device();
    if (!device->software()) {
        SKIP("the reference pictures are drawn on a software device");
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const bool updating = std::getenv("HOG_UPDATE_REFERENCES") != nullptr;
    const std::filesystem::path references =
        std::filesystem::path{HOG_SOURCE_DIR} / "apps" / "client" / "tests" / "reference";
    const std::filesystem::path failures = std::filesystem::path{HOG_BINARY_DIR} / "reference";
    for (const FixedView& view : hog::client::fixed_views()) {
        DYNAMIC_SECTION(view.name) {
            const Picture drawn = still_of(*device, view, reference_extent);
            const std::filesystem::path reference = references / std::format("{}.png", view.name);
            if (updating) {
                REQUIRE(hog::client::save_picture(reference, drawn));
                continue;
            }
            const auto expected = hog::client::load_picture(reference);
            INFO("draw it with HOG_UPDATE_REFERENCES=1 set");
            REQUIRE(expected);
            const auto difference = hog::client::difference(*expected, drawn);
            REQUIRE(difference);
            INFO("mean " << difference->mean << " counts, " << 100.0 * difference->share
                         << "% of pixels far off");
            if (!hog::client::alike(*difference)) {
                std::filesystem::create_directories(failures);
                const auto saved = failures / std::format("{}.png", view.name);
                const auto differences = failures / std::format("{}-difference.png", view.name);
                CHECK(hog::client::save_picture(saved, drawn));
                CHECK(hog::client::save_picture(differences, difference->picture));
                INFO("drawn as " << saved.string() << ", differing as " << differences.string());
                CHECK(hog::client::alike(*difference));
            }
            CHECK(device->validation_errors() == 0);
        }
    }
}
