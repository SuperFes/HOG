#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/weather_keeper.hpp>
#include <hog/sim/cell_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>

using namespace hog;
using namespace std::chrono_literals;

namespace {

globe::Planet small_earth() {
    globe::Planet planet = globe::preset("earth").value_or(globe::Planet{});
    planet.radius = 20'000.0;
    return planet;
}

constexpr int level = 2;

server::WeatherSettings quick() {
    return {.level = level, .layers = 6, .interval = 1min, .longest = 30min, .spin_up = 0s};
}

constexpr GameTime start{GameDuration{std::chrono::hours{24} * 365 * 30}};

// Polls until a job that finished has made its weather.
std::optional<protocol::Weather> next(server::WeatherKeeper& keeper, GameTime now) {
    for (int tries = 0; tries < 10'000; ++tries) {
        if (auto made = keeper.poll(now, 500ms)) {
            return made;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("the keeper steps the air only once an interval has gone by", "[server][weather]") {
    const globe::Generator generator{small_earth()};
    server::WeatherKeeper keeper{generator, 4h, 23.44, start, quick()};
    CHECK_FALSE(keeper.poll(start, 500ms));
    CHECK_FALSE(keeper.poll(start + 30s, 500ms));
    CHECK_FALSE(keeper.latest());
}

TEST_CASE("the weather it makes has a cell for each of the level's, in the grid's order",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    server::WeatherKeeper keeper{generator, 4h, 23.44, start, quick()};
    REQUIRE_FALSE(keeper.poll(start + 2min, 500ms));
    const auto found = next(keeper, start + 2min);
    REQUIRE(found);
    const protocol::Weather made = found.value_or(protocol::Weather{});
    CHECK(made.level == level);
    CHECK(made.fade == 500ms);
    REQUIRE(made.cells.size() == protocol::weather_cells(level));
    CHECK(keeper.latest() == found);

    // Cold polar cells come out as snow and warm equatorial ones mostly as rain, wherever they're
    // placed; mountains make some equatorial ground cold.
    const sim::CellGrid grid{level};
    double polar = 0.0;
    double tropic = 0.0;
    int poles = 0;
    int tropics = 0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const double z = planet::cell_center(grid.cell(index)).z;
        const protocol::CellWeather& cell = made.cells[index];
        REQUIRE(std::isfinite(cell.cloud));
        if (z > 0.9) {
            polar += static_cast<double>(cell.snow);
            ++poles;
        } else if (std::abs(z) < 0.2) {
            tropic += static_cast<double>(cell.snow);
            ++tropics;
        }
    }
    REQUIRE(poles > 0);
    REQUIRE(tropics > 0);
    CHECK(polar / poles > 0.9);
    CHECK(tropic / tropics < 0.5);
}

TEST_CASE("a keeper far behind the clock shows nothing until it has caught up",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    auto settings = quick();
    settings.spin_up = 90min;
    server::WeatherKeeper keeper{generator, 4h, 23.44, start, settings};
    const GameTime now = start;
    // Three jobs of at most half an hour bring 90 minutes of spin-up level with the clock.
    int made = 0;
    for (int tries = 0; tries < 100'000 && made == 0; ++tries) {
        if (keeper.poll(now, 500ms)) {
            ++made;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(made == 1);
}
