#include <hog/server/weather_report.hpp>

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
    const auto weather = weather_report(grid, fields, ground(grid), config(), 600'000ms);
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
        const hog::sim::Sky sky = hog::sim::sky_over(fields, under[index], config().tuning, index);
        CHECK(cell.humidity == sky.humidity);
        CHECK(cell.fog_top == sky.fog_top);
        CHECK(cell.base == sky.base);
        CHECK(cell.top == sky.top);
    }
}

TEST_CASE("nothing falling is no snow", "[server][weather]") {
    const CellGrid grid{0};
    const auto weather =
        weather_report(grid, WeatherFields{grid.size()}, ground(grid), config(), 0ms);
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
    const auto weather = weather_report(grid, fields, ground(grid), config(), 0ms);
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
    }
    // Nothing is lost or made in averaging.
    CHECK_THAT(static_cast<double>(sent_cloud), WithinRel(cloud, 1e-3));
}
