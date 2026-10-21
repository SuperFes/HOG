#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/sea.hpp>
#include <hog/sim/weather.hpp>
#include <hog/testing/assertion_guard.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

using hog::DVec3;
using hog::Vec2;
using hog::jobs::JobSystem;
using hog::sim::CellGrid;
using hog::sim::Ground;
using hog::sim::Sea;
using hog::sim::SeaFields;
using hog::sim::SeaTuning;
using hog::sim::WaveSystem;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr double g = hog::waves::gravity;
constexpr double hour = 3'600.0;

DVec3 up_at(double latitude, double longitude) {
    return {std::cos(latitude * degree) * std::cos(longitude * degree),
            std::cos(latitude * degree) * std::sin(longitude * degree),
            std::sin(latitude * degree)};
}

std::uint32_t cell_at(const CellGrid& grid, double latitude, double longitude) {
    return grid.index(hog::planet::cell_at(up_at(latitude, longitude), grid.level()));
}

double longitude_of(const CellGrid& grid, std::uint32_t index) {
    const DVec3 up = grid.frame(index).up;
    return std::atan2(up.y, up.x) / degree;
}

// Sea everywhere but where `dry` holds.
std::vector<Ground> ground_for(const CellGrid& grid,
                               const std::function<bool(std::uint32_t)>& dry = {}) {
    std::vector<Ground> ground(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        ground[index].ocean = !dry || !dry(index);
    }
    return ground;
}

// `speed` m/s east wherever `blowing` holds, and calm elsewhere.
std::vector<Vec2> east_wind(const CellGrid& grid, double speed,
                            const std::function<bool(std::uint32_t)>& blowing = {}) {
    std::vector<Vec2> wind(grid.size(), Vec2{0.0F});
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (!blowing || blowing(index)) {
            wind[index] = Vec2{static_cast<float>(speed), 0.0F};
        }
    }
    return wind;
}

double swell_variance(const SeaFields& fields, std::uint32_t index) {
    double variance = 0.0;
    for (const float bin : fields.swell_at(index)) {
        variance += static_cast<double>(bin);
    }
    return variance;
}

double grown_height(double speed) {
    return hog::waves::grown_sea(DVec3{speed, 0.0, 0.0}).height;
}

// JONSWAP's height for a sea `seconds` under a wind of `speed`, as it grows following its peak.
double height_after(double speed, double seconds) {
    const double fetch =
        std::pow(g * seconds / (2.0 * 2.0 * std::numbers::pi * 3.5 / 0.67 * speed), 1.0 / 0.67);
    return std::min(4.0 * std::sqrt(1.6e-7 * fetch) * speed * speed / g, grown_height(speed));
}

void run(Sea& sea, const CellGrid& grid, std::span<const Vec2> wind, double hours,
         JobSystem& jobs) {
    for (double done = 0.0; done < hours; done += 1.0) {
        sea.step(grid, wind, hour, &jobs);
    }
}

} // namespace

TEST_CASE("a wind sea grows with time as JONSWAP found, to Pierson and Moskowitz's", "[sim][sea]") {
    JobSystem jobs{1};
    const CellGrid grid{3};
    Sea sea{grid, ground_for(grid), SeaTuning{}};
    const std::vector<Vec2> wind = east_wind(grid, 15.0);
    const std::uint32_t middle = cell_at(grid, 10.0, 30.0);
    double last_peak = 1e9;
    for (const double hours : {3.0, 6.0, 12.0}) {
        sea.raise(grid, wind, 0.0);
        run(sea, grid, wind, hours, jobs);
        const WaveSystem grown = sea.wind_sea(middle);
        CAPTURE(hours);
        CHECK(std::abs(static_cast<double>(grown.height) - height_after(15.0, hours * hour)) <
              height_after(15.0, hours * hour) * 0.02);
        CHECK(static_cast<double>(grown.height) < grown_height(15.0));
        CHECK(static_cast<double>(grown.peak) < last_peak);
        CHECK(grown.heading == Vec2{1.0F, 0.0F});
        last_peak = static_cast<double>(grown.peak);
    }
    run(sea, grid, wind, 72.0, jobs);
    const WaveSystem full = sea.wind_sea(middle);
    CHECK(std::abs(static_cast<double>(full.height) - grown_height(15.0)) <
          grown_height(15.0) * 1e-3);
    CHECK(std::abs(static_cast<double>(full.peak) -
                   hog::waves::grown_sea(DVec3{15.0, 0.0, 0.0}).peak) < 1e-3);
    // A steady wind leaves no swell.
    CHECK(swell_variance(sea.fields(), middle) < 1e-4);
}

TEST_CASE("a wind sea grows with its distance from the coast upwind", "[sim][sea]") {
    JobSystem jobs{4};
    const CellGrid grid{5};
    // Land between 40° west and the meridian, and a gale blowing off it.
    const std::vector<Ground> ground = ground_for(grid, [&](std::uint32_t index) {
        const double longitude = longitude_of(grid, index);
        return longitude > -40.0 && longitude < 0.0;
    });
    Sea sea{grid, ground, SeaTuning{}};
    const std::vector<Vec2> wind = east_wind(grid, 30.0);
    sea.raise(grid, wind, 0.0);
    run(sea, grid, wind, 60.0, jobs);
    double last = 0.0;
    for (const double longitude : {2.0, 6.0, 10.0, 14.0}) {
        const std::uint32_t index = cell_at(grid, 0.0, longitude);
        REQUIRE(ground[index].ocean);
        const auto height = static_cast<double>(sea.wind_sea(index).height);
        CAPTURE(longitude, height);
        CHECK(height > last);
        last = height;
    }
    CHECK(static_cast<double>(sea.wind_sea(cell_at(grid, 0.0, 2.0)).height) <
          0.8 * grown_height(30.0));
    CHECK(std::abs(static_cast<double>(sea.wind_sea(cell_at(grid, 0.0, 90.0)).height) -
                   grown_height(30.0)) < grown_height(30.0) * 0.01);
}

TEST_CASE("a sea its wind leaves runs on as swell", "[sim][sea]") {
    JobSystem jobs{1};
    const CellGrid grid{3};
    Sea sea{grid, ground_for(grid), SeaTuning{}};
    const std::vector<Vec2> blowing = east_wind(grid, 20.0);
    sea.raise(grid, blowing, 4.0 * 24.0 * hour);
    const std::uint32_t middle = cell_at(grid, 0.0, 30.0);
    const double raised = std::pow(static_cast<double>(sea.wind_sea(middle).height) / 4.0, 2.0);
    sea.step(grid, east_wind(grid, 0.0), 60.0, &jobs);
    CHECK(sea.wind_sea(middle).height == 0.0F);
    CHECK(swell_variance(sea.fields(), middle) > 0.95 * raised);
    const auto swells = hog::sim::strongest_swells(sea.fields().swell_at(middle));
    CHECK(swells[0].heading.x > 0.99F);
    CHECK(std::abs(static_cast<double>(swells[0].peak) -
                   hog::waves::grown_sea(DVec3{20.0, 0.0, 0.0}).peak) < 0.1);

    // Turned across, the wind holds up none of it, and raises a new sea its own way.
    Sea turned{grid, ground_for(grid), SeaTuning{}};
    turned.raise(grid, blowing, 4.0 * 24.0 * hour);
    std::vector<Vec2> north(grid.size(), Vec2{0.0F, 20.0F});
    turned.step(grid, north, 600.0, &jobs);
    CHECK(turned.wind_sea(middle).heading == Vec2{0.0F, 1.0F});
    CHECK(static_cast<double>(turned.wind_sea(middle).height) < 0.2 * 4.0 * std::sqrt(raised));
    CHECK(swell_variance(turned.fields(), middle) > 0.95 * raised);
}

TEST_CASE("swell runs on from a storm along great circles, the longest first", "[sim][sea]") {
    JobSystem jobs{4};
    const CellGrid grid{4};
    Sea sea{grid, ground_for(grid), SeaTuning{}};
    // A gale for a day within ten degrees of the equator's crossing of the meridian.
    const std::vector<Vec2> storm = east_wind(grid, 25.0, [&](std::uint32_t index) {
        return glm::dot(grid.frame(index).up, DVec3{1.0, 0.0, 0.0}) > std::cos(10.0 * degree);
    });
    run(sea, grid, storm, 24.0, jobs);
    const std::vector<Vec2> calm = east_wind(grid, 0.0);
    const std::uint32_t ahead = cell_at(grid, 0.0, 45.0);
    const std::uint32_t behind = cell_at(grid, 0.0, -45.0);
    const std::uint32_t aside = cell_at(grid, 45.0, 0.0);
    double most = 0.0;
    double first_peak = 0.0;
    double last_peak = 0.0;
    double behind_most = 0.0;
    double aside_most = 0.0;
    for (int hours = 0; hours < 240; ++hours) {
        sea.step(grid, calm, hour, &jobs);
        const double arrived = swell_variance(sea.fields(), ahead);
        const auto swells = hog::sim::strongest_swells(sea.fields().swell_at(ahead));
        if (first_peak == 0.0 && arrived > 0.02) {
            first_peak = static_cast<double>(swells[0].peak);
        }
        if (arrived > 0.02) {
            last_peak = static_cast<double>(swells[0].peak);
        }
        most = std::max(most, arrived);
        behind_most = std::max(behind_most, swell_variance(sea.fields(), behind));
        aside_most = std::max(aside_most, swell_variance(sea.fields(), aside));
    }
    CAPTURE(most, behind_most, aside_most, first_peak, last_peak);
    CHECK(most > 0.1);
    CHECK(behind_most < most * 0.05);
    CHECK(aside_most < most * 0.3);
    // The longest outrun the rest, so the swell's period shortens as it passes.
    CHECK(first_peak > 0.0);
    CHECK(first_peak < last_peak);
}

TEST_CASE("land shelters the sea behind it from swell", "[sim][sea]") {
    JobSystem jobs{4};
    const CellGrid grid{4};
    const auto stormy = [&](std::uint32_t index) {
        return glm::dot(grid.frame(index).up, DVec3{1.0, 0.0, 0.0}) > std::cos(10.0 * degree);
    };
    const auto swell_after = [&](const std::vector<Ground>& ground) {
        Sea sea{grid, ground, SeaTuning{}};
        run(sea, grid, east_wind(grid, 25.0, stormy), 12.0, jobs);
        const std::vector<Vec2> calm = east_wind(grid, 0.0);
        double most = 0.0;
        for (int hours = 0; hours < 150; ++hours) {
            sea.step(grid, calm, hour, &jobs);
            most = std::max(most, swell_variance(sea.fields(), cell_at(grid, 0.0, 50.0)));
        }
        return most;
    };
    const double open = swell_after(ground_for(grid));
    const double sheltered = swell_after(ground_for(grid, [&](std::uint32_t index) {
        const double longitude = longitude_of(grid, index);
        const double latitude = std::asin(grid.frame(index).up.z) / degree;
        return longitude > 25.0 && longitude < 32.0 && std::abs(latitude) < 20.0;
    }));
    CAPTURE(open, sheltered);
    CHECK(open > 0.05);
    CHECK(sheltered < open * 0.2);
}

TEST_CASE("the strongest swells are told apart by heading", "[sim][sea]") {
    std::vector<float> swell(hog::sim::swell_bins);
    const auto add = [&](int heading, double height, double peak) {
        const auto banded =
            hog::waves::band_variances({.height = height, .peak = peak, .peakedness = 7.0});
        for (int band = 0; band < hog::sim::swell_bands; ++band) {
            swell[static_cast<std::size_t>((band * hog::sim::swell_headings) + heading)] +=
                static_cast<float>(banded.at(static_cast<std::size_t>(band)));
        }
    };
    add(0, 3.0, 0.5);
    add(6, 2.0, 0.8);
    const auto swells = hog::sim::strongest_swells(swell);
    CHECK(std::abs(swells[0].height - 3.0F) < 0.1F);
    CHECK(swells[0].heading.x > 0.999F);
    CHECK(std::abs(swells[0].peak - 0.5F) < 0.05F);
    CHECK(std::abs(swells[1].height - 2.0F) < 0.1F);
    CHECK(swells[1].heading.x < -0.999F);
    CHECK(std::abs(swells[1].peak - 0.8F) < 0.08F);

    const auto none = hog::sim::strongest_swells(std::vector<float>(hog::sim::swell_bins));
    CHECK(none[0].height == 0.0F);
    CHECK(none[1].height == 0.0F);
}

TEST_CASE("the sea comes out the same on any number of threads", "[sim][sea]") {
    JobSystem one{1};
    JobSystem many{4};
    const CellGrid grid{3};
    const std::vector<Ground> ground = ground_for(
        grid, [&](std::uint32_t index) { return std::abs(longitude_of(grid, index)) < 20.0; });
    Sea a{grid, ground, SeaTuning{}};
    Sea b{grid, ground, SeaTuning{}};
    const std::vector<Vec2> wind =
        east_wind(grid, 18.0, [&](std::uint32_t index) { return grid.frame(index).up.y > 0.5; });
    run(a, grid, wind, 12.0, one);
    run(b, grid, wind, 12.0, many);
    CHECK(a.fields() == b.fields());
    CHECK(std::ranges::any_of(a.fields().swell, [](float swell) { return swell > 0.0F; }));
}
