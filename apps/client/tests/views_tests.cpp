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

TEST_CASE("a fixed view is drawn the same each time", "[client][views][gpu]") {
    const auto device = test_device();
    const Picture first = still_of(*device, view_named("islands-noon"));
    const Picture again = still_of(*device, view_named("islands-noon"));
    const auto difference = hog::client::difference(first, again);
    REQUIRE(difference);
    CHECK(difference->mean == 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the islands are darker at night than at noon", "[client][views][gpu]") {
    const auto device = test_device();
    const double noon = brightness(still_of(*device, view_named("islands-noon")));
    const double night = brightness(still_of(*device, view_named("islands-night")));
    INFO("noon " << noon << ", night " << night);
    CHECK(night < 0.5 * noon);
    CHECK(night > 0.0);
    CHECK(device->validation_errors() == 0);
}
