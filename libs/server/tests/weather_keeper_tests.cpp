#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/planet/projection.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/weather_keeper.hpp>
#include <hog/sim/cell_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
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

TEST_CASE("a keeper given a save carries on from the air in it", "[server][weather]") {
    const globe::Generator generator{small_earth()};
    server::WeatherKeeper first{generator, 4h, 23.44, start, quick()};
    REQUIRE_FALSE(first.poll(start + 2min, 500ms));
    const auto made = next(first, start + 2min);
    REQUIRE(made);
    REQUIRE_FALSE(first.saved().empty());

    server::WeatherKeeper second{generator, 4h, 23.44, start + 2min, quick()};
    REQUIRE(second.restore(first.saved(), start + 2min));
    // Level with the clock, so the weather is there to be told to a joiner at once.
    const auto kept = second.latest().value_or(protocol::Weather{});
    const auto original = made.value_or(protocol::Weather{});
    REQUIRE(second.latest());
    REQUIRE(kept.cells.size() == original.cells.size());
    CHECK(kept.cells.front().cloud == original.cells.front().cloud);
}

TEST_CASE("a save of another grid is refused", "[server][weather]") {
    const globe::Generator generator{small_earth()};
    server::WeatherKeeper first{generator, 4h, 23.44, start, quick()};
    REQUIRE(next(first, start + 2min));
    auto other = quick();
    other.layers = 4;
    server::WeatherKeeper second{generator, 4h, 23.44, start, other};
    CHECK_FALSE(second.restore(first.saved(), start + 2min));
    CHECK_FALSE(second.latest());
}

// Spins the real planet's air up and writes the weather it makes where HOG_WEATHER says, for the
// client's stills to be drawn under.
TEST_CASE("the weather after a spin-up, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_WEATHER");
    REQUIRE(where != nullptr);
    globe::Planet planet = globe::preset("earth").value_or(globe::Planet{});
    planet.radius = 115'000.0;
    const globe::Generator generator{planet};
    server::WeatherSettings settings{};
    settings.spin_up = 48h;
    server::WeatherKeeper keeper{generator, 4h, 23.44, start, settings};
    std::optional<protocol::Weather> found;
    while (!found) {
        found = keeper.poll(start, 500ms);
        std::this_thread::sleep_for(100ms);
    }
    const auto bytes = protocol::encode(protocol::Message{found.value_or(protocol::Weather{})});
    std::ofstream out{where, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}
