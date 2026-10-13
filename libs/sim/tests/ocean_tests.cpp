#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/ocean.hpp>
#include <hog/sim/weather.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using hog::DVec3;
using hog::Vec2;
using hog::sim::CellGrid;
using hog::sim::Ground;
using hog::sim::Ocean;
using hog::sim::OceanTuning;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr int level = 4;
constexpr double rotation = 2.0 * std::numbers::pi / 86'400.0;

const CellGrid& grid() {
    static const CellGrid grid{level};
    return grid;
}

double latitude(std::uint32_t index) {
    return std::asin(grid().frame(index).up.z) / degree;
}

double longitude(std::uint32_t index) {
    const DVec3 up = grid().frame(index).up;
    return std::atan2(up.y, up.x) / degree;
}

std::uint32_t cell_at(double lat, double lon) {
    return grid().index(
        hog::planet::cell_at(hog::planet::direction_at(lat * degree, lon * degree), level));
}

// A basin 120° wide reaching 60° either side of the equator, closed by land all round.
std::vector<Ground> basin() {
    std::vector<Ground> ground(grid().size());
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        ground[index].ocean = std::abs(latitude(index)) < 60.0 && std::abs(longitude(index)) < 60.0;
    }
    return ground;
}

// Easterly trades near the equator and westerlies near 45°, as Earth's.
std::vector<Vec2> belts() {
    std::vector<Vec2> wind(grid().size());
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        wind[index] = Vec2{-7.0 * std::cos(3.0 * latitude(index) * degree), 0.0};
    }
    return wind;
}

Ocean settled(const std::vector<Ground>& ground, const std::vector<Vec2>& wind) {
    Ocean ocean{grid(), ground, OceanTuning{}, rotation, 0.0};
    ocean.settle_under(grid(), wind, nullptr);
    return ocean;
}

const Ocean& gyres() {
    static const Ocean ocean = settled(basin(), belts());
    return ocean;
}

Vec2 current(double lat, double lon) {
    return gyres().fields().current[cell_at(lat, lon)];
}

// The ocean cell at a latitude nearest the basin's western or eastern coast.
std::uint32_t coast(double lat, double towards) {
    const auto ground = basin();
    double lon = 70.0 * towards;
    while (!ground[cell_at(lat, lon)].ocean) {
        lon -= towards;
    }
    return cell_at(lat, lon);
}

Vec2 coastal(double lat, double towards) {
    return gyres().fields().current[coast(lat, towards)];
}

constexpr double west = -1.0;
constexpr double east = 1.0;

} // namespace

TEST_CASE("gyres turn clockwise in the north and anticlockwise in the south", "[sim][ocean]") {
    // Poleward along the western coast, back towards the equator through the middle.
    CHECK(coastal(25.0, west).y > 0.0F);
    CHECK(current(25.0, 0.0).y < 0.0F);
    CHECK(coastal(-25.0, west).y < 0.0F);
    CHECK(current(-25.0, 0.0).y > 0.0F);
    // West with the trades, east with the westerlies.
    CHECK(current(8.0, 0.0).x < 0.0F);
    CHECK(current(40.0, 0.0).x > 0.0F);
}

TEST_CASE("gyres return fast along the western coast and slowly along the eastern",
          "[sim][ocean]") {
    for (const double lat : {-25.0, 25.0}) {
        const float western = std::abs(coastal(lat, west).y);
        const float eastern = std::abs(coastal(lat, east).y);
        CHECK(western > 3.0F * eastern);
        CHECK(western > 0.1F);
        CHECK(static_cast<double>(western) < OceanTuning{}.max_current);
    }
}

TEST_CASE("currents run along coasts, not into land", "[sim][ocean]") {
    const auto ground = basin();
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        const Vec2 flow = gyres().fields().current[index];
        if (!ground[index].ocean) {
            REQUIRE(flow == Vec2{0.0F});
        }
    }
    // The western current hugs the coast running north and south.
    for (const double lat : {-30.0, -20.0, 20.0, 30.0}) {
        const Vec2 flow = coastal(lat, west);
        CHECK(std::abs(flow.y) > 2.0F * std::abs(flow.x));
    }
}

TEST_CASE("a sea without coasts runs round the planet with the winds", "[sim][ocean]") {
    const std::vector<Ground> sea(grid().size());
    const Ocean ocean = settled(sea, belts());
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        const Vec2 flow = ocean.fields().current[index];
        REQUIRE(std::isfinite(flow.x));
        if (std::abs(latitude(index)) < 10.0) {
            CHECK(flow.x < 0.0F);
        }
        if (std::abs(latitude(index)) > 40.0 && std::abs(latitude(index)) < 50.0) {
            CHECK(flow.x > 0.0F);
        }
        CHECK(std::abs(flow.y) < 0.2F * std::abs(flow.x) + 0.01F);
    }
}

TEST_CASE("currents follow the winds slowly", "[sim][ocean]") {
    hog::jobs::JobSystem jobs{1};
    Ocean ocean = gyres();
    const std::vector<Vec2> calm(grid().size(), Vec2{0.0F});
    const auto western = [&] { return glm::length(ocean.fields().current[cell_at(25.0, -40.0)]); };
    const float before = western();
    ocean.step(grid(), calm, 0.0, 6.0 * 3'600.0, &jobs);
    CHECK(western() > 0.9F * before);
    for (int step = 0; step < 4 * 60; ++step) {
        ocean.step(grid(), calm, 0.0, 6.0 * 3'600.0, &jobs);
    }
    CHECK(western() < 0.1F * before);
}

TEST_CASE("the sea comes out the same on any number of threads", "[sim][ocean]") {
    const auto run = [&](std::uint32_t threads) {
        hog::jobs::JobSystem jobs{threads};
        Ocean ocean = gyres();
        auto wind = belts();
        for (Vec2& blowing : wind) {
            blowing.y += 3.0F;
        }
        for (int step = 0; step < 8; ++step) {
            ocean.step(grid(), wind, 0.3, 6.0 * 3'600.0, &jobs);
        }
        return ocean.fields();
    };
    CHECK(run(0) == run(3));
}

namespace {

constexpr double day = 86'400.0;

// Warm at the equator and cold at the poles, as Earth's sea.
std::vector<Ground> zonal(std::vector<Ground> ground) {
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        const double sin_latitude = grid().frame(index).up.z;
        ground[index].mean_temperature =
            static_cast<float>(28.0 - (35.0 * sin_latitude * sin_latitude));
    }
    return ground;
}

void warm_for(Ocean& ocean, int days) {
    for (int step = 0; step < days; ++step) {
        ocean.warm(grid(), 0.0, day, nullptr);
    }
}

} // namespace

TEST_CASE("currents carry warm water along western coasts and cold along eastern", "[sim][ocean]") {
    Ocean ocean = settled(zonal(basin()), belts());
    warm_for(ocean, 180);
    const auto& temperature = ocean.fields().temperature;
    for (const double lat : {-30.0, 30.0}) {
        CHECK(temperature[coast(lat, west)] > temperature[coast(lat, east)] + 1.0F);
    }
}

TEST_CASE("colder water wells up where the wind drives the sea off a coast", "[sim][ocean]") {
    // A wind towards the equator pushes the surface west, off the eastern coast, either side.
    std::vector<Vec2> wind(grid().size());
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        wind[index] = Vec2{0.0F, latitude(index) > 0.0 ? -7.0F : 7.0F};
    }
    Ocean ocean = settled(basin(), wind);
    warm_for(ocean, 180);
    const auto& temperature = ocean.fields().temperature;
    CHECK(temperature[coast(30.0, east)] < temperature[coast(30.0, west)] - 0.5F);
    CHECK(temperature[coast(-30.0, east)] < temperature[coast(-30.0, west)] - 0.5F);
    CHECK(temperature[cell_at(30.0, 0.0)] > temperature[coast(30.0, east)]);
    for (const float warmth : temperature) {
        REQUIRE(warmth <= 15.0F);
    }
}

TEST_CASE("the sea's seasons come late and mild", "[sim][ocean]") {
    const std::vector<Ground> sea(grid().size());
    const double tilt = std::sin(23.44 * degree);
    Ocean ocean{grid(), sea, OceanTuning{}, rotation, 0.0};
    const std::uint32_t north = cell_at(45.0, 0.0);
    // From the March equinox, through a year to settle and a year to watch.
    int warmest = 0;
    float high = -100.0F;
    float low = 100.0F;
    for (int date = 0; date < 2 * 365; ++date) {
        ocean.warm(grid(), tilt * std::sin(2.0 * std::numbers::pi * date / 365.25), day, nullptr);
        const float warmth = ocean.fields().temperature[north];
        if (date >= 365 && warmth > high) {
            high = warmth;
            warmest = date - 365;
        }
        if (date >= 365) {
            low = std::min(low, warmth);
        }
    }
    // The June solstice comes 91 days in.
    CHECK(warmest > 91 + 25);
    CHECK(warmest < 91 + 50);
    const double swing = OceanTuning{}.seasons * tilt * std::sin(45.0 * degree);
    CHECK(static_cast<double>(high - low) / 2.0 < swing);
    CHECK(static_cast<double>(high - low) / 2.0 > 0.6 * swing);
}

TEST_CASE("the sea ices over rather than cooling further", "[sim][ocean]") {
    std::vector<Ground> sea(grid().size());
    for (Ground& ground : sea) {
        ground.mean_temperature = -20.0F;
    }
    Ocean ocean = settled(sea, belts());
    warm_for(ocean, 10);
    for (const float warmth : ocean.fields().temperature) {
        REQUIRE(static_cast<double>(warmth) == OceanTuning{}.freezing);
    }
}

TEST_CASE("oceans reject bad input", "[sim][ocean]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS((Ocean{grid(), std::vector<Ground>(3), OceanTuning{}, rotation, 0.0}),
                    AssertionFailed);
    Ocean ocean = gyres();
    CHECK_THROWS_AS(ocean.settle_under(grid(), std::vector<Vec2>(3), nullptr), AssertionFailed);
    CHECK_THROWS_AS(ocean.step(grid(), std::vector<Vec2>(3), 0.0, 1.0, nullptr), AssertionFailed);
    CHECK_THROWS_AS(ocean.set_fields(grid(), hog::sim::OceanFields{3}), AssertionFailed);
}
