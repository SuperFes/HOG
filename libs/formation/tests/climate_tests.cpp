#include <hog/formation/climate.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::sea_level_temperature;
using hog::formation::Sphere;

namespace {

constexpr double degree = std::numbers::pi / 180.0;

double latitude_of(const DVec3& point) {
    return std::asin(point.z);
}

double longitude_of(const DVec3& point) {
    return std::atan2(point.y, point.x);
}

} // namespace

TEST_CASE("Earth's numbers give Earth-like temperatures", "[formation][climate]") {
    const Body body;
    CHECK_THAT(sea_level_temperature(body, 0.0), WithinAbs(26.0, 5.0));
    CHECK(sea_level_temperature(body, 90.0 * degree) < -10.0);
    CHECK(sea_level_temperature(body, -90.0 * degree) < -10.0);
    CHECK(sea_level_temperature(body, 45.0 * degree) > sea_level_temperature(body, 70.0 * degree));
}

TEST_CASE("a planet on its side warms its poles past its equator", "[formation][climate]") {
    Body upright;
    upright.tilt = 0.0;
    Body sideways;
    sideways.tilt = 90.0;
    const auto gap = [](const Body& body) {
        return sea_level_temperature(body, 0.0) - sea_level_temperature(body, 89.0 * degree);
    };
    CHECK(gap(upright) > gap(Body{}));
    CHECK(gap(sideways) < 0.0);
}

TEST_CASE("a brighter star warms everywhere", "[formation][climate]") {
    Body bright;
    bright.star_flux = 1'600.0;
    CHECK(sea_level_temperature(bright, 30.0 * degree) > sea_level_temperature(Body{}, 30.0 * degree));
}

TEST_CASE("ground cools with height and rain totals what's asked", "[formation][climate]") {
    const Body body;
    const Sphere sphere{5, 3};
    std::vector<double> heights(sphere.size(), -3'000.0);
    std::vector<double> high = heights;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (std::abs(longitude_of(sphere.point(point))) < 40.0 * degree) {
            heights[point] = 0.0;
            high[point] = 1'000.0;
        }
    }
    const Climate low_ground = climate_on(sphere, heights, body);
    const Climate high_ground = climate_on(sphere, high, body);
    double total = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        total += low_ground.rain[point] * sphere.area(point);
        if (heights[point] == 0.0) {
            CHECK_THAT(low_ground.temperature[point] - high_ground.temperature[point],
                       WithinAbs(1'000.0 * body.lapse_rate, 1e-9));
        }
        CHECK(low_ground.rain[point] >= 0.0);
        CHECK_THAT(glm::length(low_ground.wind[point]), WithinAbs(1.0, 1e-9));
    }
    CHECK_THAT(total / (4.0 * std::numbers::pi), WithinRel(body.rain_mean, 1e-9));
}

TEST_CASE("mountains across the wind wring the rain out before their far side",
          "[formation][climate]") {
    const Body body;
    const Sphere sphere{6, 3};
    // Land from 0° to 60° east, a ridge along 20° east, in the mid-latitude westerlies.
    std::vector<double> heights(sphere.size(), -3'000.0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double longitude = longitude_of(sphere.point(point)) / degree;
        if (longitude > 0.0 && longitude < 60.0) {
            heights[point] = 200.0 + (3'000.0 * std::exp(-std::pow((longitude - 20.0) / 4.0, 2)));
        }
    }
    const Climate climate = climate_on(sphere, heights, body);
    double windward = 0.0;
    double leeward = 0.0;
    int windward_count = 0;
    int leeward_count = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double latitude = latitude_of(sphere.point(point)) / degree;
        const double longitude = longitude_of(sphere.point(point)) / degree;
        if (latitude < 40.0 || latitude > 55.0) {
            continue;
        }
        if (longitude > 8.0 && longitude < 18.0) {
            windward += climate.rain[point];
            ++windward_count;
        } else if (longitude > 26.0 && longitude < 45.0) {
            leeward += climate.rain[point];
            ++leeward_count;
        }
    }
    REQUIRE(windward_count > 0);
    REQUIRE(leeward_count > 0);
    CHECK(windward / windward_count > 2.0 * (leeward / leeward_count));
}

TEST_CASE("spinning backwards turns the winds round", "[formation][climate]") {
    Body forwards;
    Body backwards;
    backwards.day = -24.0;
    const Sphere sphere{4, 3};
    const std::vector<double> heights(sphere.size(), -1'000.0);
    const Climate one = climate_on(sphere, heights, forwards);
    const Climate other = climate_on(sphere, heights, backwards);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (std::abs(sphere.point(point).z) < 0.95) {
            const DVec3 east = glm::normalize(glm::cross(DVec3{0, 0, 1}, sphere.point(point)));
            CHECK(glm::dot(one.wind[point], east) * glm::dot(other.wind[point], east) <= 0.0);
        }
    }
}
