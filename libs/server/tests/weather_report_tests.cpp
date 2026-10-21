#include <hog/server/weather_report.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::Vec2;
using hog::server::weather_report;
using hog::sim::CellGrid;
using hog::sim::Ground;
using hog::sim::WeatherConfig;
using hog::sim::WeatherFields;

namespace {

// Ten-minute steps, drifting with the wind 0.4 of the way to the jet's.
WeatherConfig config() {
    WeatherConfig out;
    out.step = 10min;
    out.tuning.motion.jet_steering = 0.4;
    return out;
}

// Land rising a hundred metres a cell.
std::vector<Ground> ground(const CellGrid& grid) {
    std::vector<Ground> out(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        out[index] = {.elevation = 100.0F * static_cast<float>(index), .ocean = false};
    }
    return out;
}

// A calm sea with no current, under every cell.
struct Still {
    hog::sim::SeaFields sea;
    std::vector<Vec2> current;

    explicit Still(const CellGrid& grid) : sea(grid.size()), current(grid.size(), Vec2{0.0F}) {}
};

} // namespace

TEST_CASE("weather as coarse as may be sent goes cell for cell", "[server][weather]") {
    const CellGrid grid{2};
    WeatherFields fields{grid.size()};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto value = static_cast<float>(index);
        fields.cloud[index] = value / 100.0F;
        fields.rain[index] = value / 10.0F;
        fields.snow[index] = 0.1F;
        fields.hail[index] = 0.2F;
        fields.wind[index] = {value, -5.0F};
        fields.jet[index] = {value + 10.0F, 5.0F};
        fields.temperature[index] = value - 20.0F;
        fields.vapour[index] = 5.0F;
        fields.lift[index] = (value - 40.0F) / 1'000.0F;
        fields.storm[index] = value / 200.0F;
    }
    const std::vector<Ground> under = ground(grid);
    const Still still{grid};
    const auto weather =
        weather_report(grid, fields, ground(grid), still.sea, still.current, config(), 600'000ms);
    CHECK(weather.level == 2);
    CHECK(weather.fade == 600'000ms);
    REQUIRE(weather.cells.size() == grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        CAPTURE(index);
        const auto value = static_cast<double>(index);
        const auto& cell = weather.cells[index];
        CHECK_THAT(static_cast<double>(cell.cloud), WithinAbs(value / 100.0, 1e-6));
        // Ten minutes' worth, six times over in an hour.
        const double falling = 6.0 * ((value / 10.0) + 0.3);
        CHECK_THAT(static_cast<double>(cell.falling), WithinRel(falling, 1e-5));
        CHECK_THAT(static_cast<double>(cell.snow), WithinRel(0.6 / falling, 1e-5));
        CHECK_THAT(static_cast<double>(cell.drift.x), WithinAbs(value + 4.0, 1e-4));
        CHECK_THAT(static_cast<double>(cell.drift.y), WithinAbs(-1.0, 1e-5));
        CHECK(cell.wind == fields.wind[index]);
        const hog::sim::Sky sky = hog::sim::sky_over(fields, under[index], config().tuning, index);
        CHECK(cell.humidity == sky.humidity);
        CHECK(cell.fog_top == sky.fog_top);
        CHECK(cell.base == sky.base);
        CHECK(cell.top == sky.top);
    }
}

TEST_CASE("nothing falling is no snow", "[server][weather]") {
    const CellGrid grid{0};
    const auto weather = weather_report(grid, WeatherFields{grid.size()}, ground(grid),
                                        Still{grid}.sea, Still{grid}.current, config(), 0ms);
    for (const auto& cell : weather.cells) {
        CHECK(cell.falling == 0.0F);
        CHECK(cell.snow == 0.0F);
    }
}

TEST_CASE("finer weather is averaged over the cells sent", "[server][weather]") {
    const CellGrid grid{hog::protocol::most_weather_level + 1};
    const CellGrid sent{hog::protocol::most_weather_level};
    WeatherFields fields{grid.size()};
    // Under each cell sent, one of its four snows a millimetre a step and the rest rain three.
    double cloud = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const hog::planet::CellId cell = grid.cell(index);
        const bool first = cell.parent().child(0) == cell;
        fields.cloud[index] = first ? 2.0F : 0.0F;
        fields.snow[index] = first ? 1.0F : 0.0F;
        fields.rain[index] = first ? 0.0F : 3.0F;
        fields.wind[index] = {first ? 8.0F : 0.0F, 1.0F};
        fields.jet[index] = fields.wind[index];
        cloud += grid.area(index) * static_cast<double>(fields.cloud[index]);
    }
    const Still still{grid};
    const auto weather =
        weather_report(grid, fields, ground(grid), still.sea, still.current, config(), 0ms);
    CHECK(weather.level == hog::protocol::most_weather_level);
    REQUIRE(weather.cells.size() == sent.size());
    double sent_cloud = 0.0;
    for (std::uint32_t index = 0; index < sent.size(); ++index) {
        CAPTURE(index);
        const auto& cell = weather.cells[index];
        sent_cloud += sent.area(index) * static_cast<double>(cell.cloud);
        // The four under a cell are near enough the same size.
        CHECK_THAT(static_cast<double>(cell.cloud), WithinRel(0.5, 0.05));
        CHECK_THAT(static_cast<double>(cell.falling), WithinRel(6.0 * 10.0 / 4.0, 0.05));
        CHECK_THAT(static_cast<double>(cell.snow), WithinRel(0.1, 0.05));
        CHECK_THAT(static_cast<double>(cell.drift.x), WithinRel(2.0, 0.05));
        CHECK_THAT(static_cast<double>(cell.drift.y), WithinAbs(1.0, 1e-5));
        CHECK_THAT(static_cast<double>(cell.wind.x), WithinRel(2.0, 0.05));
        CHECK_THAT(static_cast<double>(cell.wind.y), WithinAbs(1.0, 1e-5));
    }
    // Nothing is lost or made in averaging.
    CHECK_THAT(static_cast<double>(sent_cloud), WithinRel(cloud, 1e-3));
}

TEST_CASE("the sea's waves and currents go with the weather", "[server][weather]") {
    const CellGrid grid{hog::protocol::most_weather_level + 1};
    const CellGrid sent{hog::protocol::most_weather_level};
    Still still{grid};
    // Under each cell sent, one of its four has a wind sea running north and a swell east, and
    // each of them a current.
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const hog::planet::CellId cell = grid.cell(index);
        still.current[index] = {0.4F, -0.2F};
        if (cell.parent().child(0) != cell) {
            continue;
        }
        still.sea.wind_sea[index] = 4.0F;
        still.sea.peak[index] = 0.7F;
        still.sea.heading[index] = {0.0F, 1.0F};
        const auto banded =
            hog::waves::band_variances({.height = 4.0, .peak = 0.5, .peakedness = 7.0});
        for (int band = 0; band < hog::sim::swell_bands; ++band) {
            still.sea.swell[(index * hog::sim::swell_bins) +
                            static_cast<std::size_t>(band * hog::sim::swell_headings)] =
                static_cast<float>(banded.at(static_cast<std::size_t>(band)));
        }
    }
    const auto weather = weather_report(grid, WeatherFields{grid.size()}, ground(grid), still.sea,
                                        still.current, config(), 0ms);
    REQUIRE(weather.cells.size() == sent.size());
    for (std::uint32_t index = 0; index < sent.size(); ++index) {
        CAPTURE(index);
        const auto& cell = weather.cells[index];
        // A quarter of the variance, so half the height.
        CHECK_THAT(static_cast<double>(cell.wind_sea.height), WithinRel(4.0 * 2.0 / 2.0, 0.05));
        CHECK_THAT(static_cast<double>(cell.wind_sea.peak), WithinRel(0.7, 1e-5));
        CHECK(cell.wind_sea.heading == Vec2{0.0F, 1.0F});
        CHECK_THAT(static_cast<double>(cell.swells[0].height), WithinRel(4.0 / 2.0, 0.05));
        CHECK_THAT(static_cast<double>(cell.swells[0].peak), WithinRel(0.5, 0.1));
        CHECK(cell.swells[0].heading.x > 0.999F);
        CHECK(cell.swells[1].height == 0.0F);
        CHECK_THAT(static_cast<double>(cell.current.x), WithinRel(0.4, 1e-5));
        CHECK_THAT(static_cast<double>(cell.current.y), WithinRel(-0.2, 1e-5));
    }
}
