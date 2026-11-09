#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/planet/projection.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/skies/weather_keeper.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace hog;
using namespace std::chrono_literals;

namespace {

globe::Planet small_earth() {
    globe::Planet planet;
    planet.body.radius = 20'000.0;
    return planet;
}

constexpr int level = 2;

skies::WeatherSettings quick() {
    return {.level = level, .layers = 6, .interval = 1min, .longest = 30min, .spin_up = 0s};
}

constexpr GameTime start{GameDuration{std::chrono::hours{24} * 365 * 30}};

// Polls until a job that finished has made its weather.
std::optional<protocol::Weather> next(skies::WeatherKeeper& keeper, GameTime now) {
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
    skies::WeatherKeeper keeper{generator, start, quick()};
    CHECK_FALSE(keeper.poll(start, 500ms));
    CHECK_FALSE(keeper.poll(start + 30s, 500ms));
    CHECK_FALSE(keeper.latest());
}

TEST_CASE("the weather it makes has a cell for each of the level's, in the grid's order",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    skies::WeatherKeeper keeper{generator, start, quick()};
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

TEST_CASE("the wind raises waves on the sea and none on land", "[server][weather]") {
    const globe::Generator generator{small_earth()};
    skies::WeatherKeeper keeper{generator, start, quick()};
    REQUIRE_FALSE(keeper.poll(start + 2min, 500ms));
    const auto found = next(keeper, start + 2min);
    REQUIRE(found);
    const protocol::Weather made = found.value_or(protocol::Weather{});
    int seas = 0;
    for (const protocol::CellWeather& cell : made.cells) {
        const bool windy = glm::length(cell.wind) > 1.0F;
        if (cell.wind_sea.height > 0.0F) {
            ++seas;
            CHECK(cell.wind_sea.peak > 0.0F);
            CHECK(glm::length(cell.wind_sea.heading) == Catch::Approx(1.0).margin(1e-3));
            // A stronger wind raises higher waves than any under it could: not above Pierson and
            // Moskowitz's 0.0214 U² metres, and not nothing.
            CHECK(cell.wind_sea.height <= 0.03F * glm::dot(cell.wind, cell.wind));
        } else if (!windy) {
            CHECK(cell.wind_sea.peak == 0.0F);
        }
    }
    CHECK(seas > 0);
    CHECK(seas < static_cast<int>(made.cells.size()));
}

TEST_CASE("a keeper far behind the clock shows nothing until it has caught up",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    auto settings = quick();
    settings.spin_up = 90min;
    skies::WeatherKeeper keeper{generator, start, settings};
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
    skies::WeatherKeeper first{generator, start, quick()};
    REQUIRE_FALSE(first.poll(start + 2min, 500ms));
    const auto made = next(first, start + 2min);
    REQUIRE(made);
    REQUIRE_FALSE(first.saved().empty());

    skies::WeatherKeeper second{generator, start + 2min, quick()};
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
    skies::WeatherKeeper first{generator, start, quick()};
    REQUIRE(next(first, start + 2min));
    auto other = quick();
    other.layers = 4;
    skies::WeatherKeeper second{generator, start, other};
    CHECK_FALSE(second.restore(first.saved(), start + 2min));
    CHECK_FALSE(second.latest());
}

// Spins the real planet's air up and writes the weather it makes where HOG_WEATHER says, for the
// client's stills to be drawn under.
TEST_CASE("the weather after a spin-up, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_WEATHER");
    REQUIRE(where != nullptr);
    globe::Planet planet;
    planet.body.radius = 115'000.0;
    const globe::Generator generator{planet};
    skies::WeatherSettings settings{};
    settings.spin_up = 48h;
    skies::WeatherKeeper keeper{generator, start, settings};
    std::optional<protocol::Weather> found;
    while (!found) {
        found = keeper.poll(start, 500ms);
        std::this_thread::sleep_for(100ms);
    }
    const auto bytes = protocol::encode(protocol::Message{found.value_or(protocol::Weather{})});
    std::ofstream out{where, std::ios::binary};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stream takes chars
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

TEST_CASE("a keeper the clock outruns still shows each job's weather, and skips ahead past its lag",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    auto settings = quick();
    settings.lag = 1h;
    skies::WeatherKeeper keeper{generator, start, settings};
    // The clock is a day ahead of the air, which is never to catch up.
    GameTime now = start + 24h;
    int made = 0;
    float pace = 0.0F;
    for (int tries = 0; tries < 100'000 && made < 3; ++tries) {
        if (const auto weather = keeper.poll(now, 500ms)) {
            ++made;
            pace = weather->pace;
            // Each job's worth is shown though the air is hours behind the clock.
            CHECK(weather->cells.size() == protocol::weather_cells(level));
        }
        std::this_thread::sleep_for(1ms);
        now += 30min;
    }
    CHECK(made == 3);
    // After the first, the pace is game seconds per real second: far over a real-time one.
    CHECK(pace > 10.0F);
}

TEST_CASE("a fade longer than the most a weather is faded over is kept as asked",
          "[server][weather]") {
    const globe::Generator generator{small_earth()};
    skies::WeatherKeeper keeper{generator, start, quick()};
    GameTime now = start + 2min;
    int made = 0;
    for (int tries = 0; tries < 100'000 && made < 2; ++tries) {
        if (const auto weather = keeper.poll(now, 5min)) {
            ++made;
            CHECK(weather->fade >= 5min);
        }
        std::this_thread::sleep_for(1ms);
        now += 2min;
    }
    CHECK(made == 2);
}
