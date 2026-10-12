#include <hog/jobs/job_system.hpp>
#include <hog/sim/forest.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::GameTime;
using hog::jobs::JobSystem;
using hog::planet::Biome;
using hog::sim::DrynessConfig;
using hog::sim::Forest;
using hog::sim::ForestConditions;
using hog::sim::ForestConfig;
using hog::sim::Soil;
using hog::sim::Stage;

namespace {

constexpr int level = 3;
constexpr GameTime start{sys_days{2026y / June / 1}.time_since_epoch()};

std::size_t at(Biome biome) {
    return static_cast<std::size_t>(biome);
}

ForestConfig config() {
    ForestConfig config{.level = level, .step = hours{24}};
    auto& growth = config.vegetation.growth;
    growth.capacity[at(Biome::TemperateForest)] = 20.0;
    growth.capacity[at(Biome::Grassland)] = 0.5;
    growth.years[at(Biome::TemperateForest)] = 60.0;
    growth.years[at(Biome::Grassland)] = 1.0;
    return config;
}

// Forest everywhere but a band of ocean and one of grassland.
std::vector<Soil> soil() {
    const hog::sim::CellGrid grid{level};
    std::vector<Soil> soil(grid.size(), Soil{.biome = Biome::TemperateForest, .moisture = 0.6F});
    for (std::uint32_t cell = 0; cell < grid.size(); cell += 5) {
        soil[cell].biome = Biome::Ocean;
    }
    for (std::uint32_t cell = 1; cell < grid.size(); cell += 5) {
        soil[cell].biome = Biome::Grassland;
    }
    return soil;
}

// The same weather in every cell.
struct Weather {
    std::vector<float> temperature;
    std::vector<float> precipitation;
    std::vector<float> rain;
    std::vector<float> storm;
    std::vector<hog::Vec2> wind;

    Weather(std::size_t size, float degrees, float recent_rain)
        : temperature(size, degrees), precipitation(size, recent_rain), rain(size), storm(size),
          wind(size, hog::Vec2{0.0F}) {}

    [[nodiscard]] ForestConditions conditions() const {
        return ForestConditions{.temperature = temperature,
                                .precipitation = precipitation,
                                .rain = rain,
                                .rain_days = 1.0,
                                .storm = storm,
                                .wind = wind};
    }
};

// Hot, with no rain for weeks: dry enough for the forest to burn.
Weather drought(std::size_t size) {
    return Weather{size, 30.0F, 0.1F};
}

// Warm and wet enough to grow at full speed.
Weather good_year(std::size_t size) {
    return Weather{size, 20.0F, 3.0F};
}

// Forest everywhere.
std::vector<Soil> woods() {
    return std::vector<Soil>(hog::sim::CellGrid{level}.size(),
                             Soil{.biome = Biome::TemperateForest, .moisture = 0.6F});
}

// A cell in the middle of the first face.
std::uint32_t middle() {
    constexpr std::uint32_t side = 1U << static_cast<unsigned>(level);
    return ((side / 2) * side) + (side / 2);
}

// How far east of `from` a cell lies, in radians.
double east_of(const hog::sim::CellGrid& grid, std::uint32_t from, std::uint32_t cell) {
    return glm::dot(grid.frame(cell).up - grid.frame(from).up, grid.frame(from).east);
}

} // namespace

TEST_CASE("a forest starts fully grown on what its biomes hold", "[sim][forest]") {
    const Forest forest{config(), soil(), start + hours{5}};
    CHECK(forest.time() == start);
    CHECK(forest.next_step() == start + hours{24});
    CHECK(forest.capacity(0) == 0.0);
    CHECK(forest.capacity(1) == 0.5);
    CHECK(forest.capacity(2) == 20.0);
    CHECK(forest.fields().biomass[2] == 20.0F);
    CHECK(forest.stage(2) == Stage::Mature);
    CHECK(forest.stage(0) == Stage::Bare);

    double kg = 0.0;
    for (std::uint32_t cell = 0; cell < forest.grid().size(); ++cell) {
        kg += forest.capacity(cell) * forest.area(cell) * 1e6;
    }
    CHECK_THAT(forest.total(), WithinRel(kg, 1e-6));
}

TEST_CASE("a grown forest stays at what its biome holds", "[sim][forest]") {
    Forest forest{config(), soil(), start};
    JobSystem jobs{2};
    const Weather summer{forest.grid().size(), 20.0F, 3.0F};
    CHECK(forest.advance_to(start + days{365}, summer.conditions(), jobs) == 365);
    CHECK(forest.time() == start + days{365});
    for (std::uint32_t cell = 0; cell < forest.grid().size(); ++cell) {
        CHECK_THAT(forest.fields().biomass[cell], WithinRel(forest.capacity(cell), 1e-5));
    }
    CHECK(forest.advance_to(start + days{365} + hours{23}, summer.conditions(), jobs) == 0);
}

TEST_CASE("the ground dries with heat and wets with rain and a wet climate", "[sim][forest]") {
    const DrynessConfig settings;
    using hog::sim::dryness;
    // Frozen ground loses nothing to the air, so only the climate dries it.
    CHECK_THAT(dryness(settings, 0.6, -5.0, 0.0), WithinAbs(0.2, 1e-9));
    CHECK(dryness(settings, 0.6, 25.0, 0.0) > dryness(settings, 0.6, 25.0, 2.0));
    CHECK(dryness(settings, 0.6, 30.0, 2.0) > dryness(settings, 0.6, 10.0, 2.0));
    CHECK(dryness(settings, 0.1, 20.0, 2.0) > dryness(settings, 0.8, 20.0, 2.0));
    CHECK_THAT(dryness(settings, 0.0, 40.0, 0.0), WithinAbs(1.0, 1e-9));
    CHECK_THAT(dryness(settings, 1.0, 0.0, 10.0), WithinAbs(0.0, 1e-9));

    Forest forest{config(), soil(), start};
    JobSystem jobs{1};
    const Weather drought{forest.grid().size(), 30.0F, 0.1F};
    forest.advance_to(forest.next_step(), drought.conditions(), jobs);
    CHECK_THAT(forest.fields().dryness[2], WithinAbs(dryness(settings, 0.6, 30.0, 0.1), 1e-6));
}

TEST_CASE("stages follow the share of the biome's plants grown back", "[sim][forest]") {
    using hog::sim::stage_of;
    CHECK(stage_of(0.0, 0.0) == Stage::Bare);
    CHECK(stage_of(0.4, 20.0) == Stage::Bare);
    CHECK(stage_of(1.9, 20.0) == Stage::Bare);
    CHECK(stage_of(4.0, 20.0) == Stage::Grass);
    CHECK(stage_of(6.0, 20.0) == Stage::Shrub);
    CHECK(stage_of(12.0, 20.0) == Stage::Young);
    CHECK(stage_of(19.0, 20.0) == Stage::Mature);
    CHECK(hog::sim::to_string(Stage::Shrub) == "shrub");
}

TEST_CASE("a fire burns its cell down and the plants grow back through their stages",
          "[sim][forest]") {
    ForestConfig settings = config();
    settings.vegetation.fire.spread = 0.0;
    Forest forest{settings, woods(), start};
    JobSystem jobs{2};
    const std::uint32_t cell = middle();
    // Nothing burns until the ground has been seen to dry out.
    CHECK_FALSE(forest.ignite(cell));
    const Weather dry = drought(forest.grid().size());
    forest.advance_to(forest.next_step(), dry.conditions(), jobs);
    REQUIRE(forest.ignite(cell));
    CHECK(forest.burning() == 1);
    CHECK(forest.fields().fire[cell] > 0.0F);
    const hog::sim::FireReport lit = forest.drain_fires();
    CHECK(lit.started == 1);
    CHECK(lit.alight == forest.area(cell));
    CHECK(forest.drain_fires() == hog::sim::FireReport{});

    forest.advance_to(forest.time() + days{3}, dry.conditions(), jobs);
    CHECK(forest.burning() == 0);
    CHECK(forest.stage(cell) == Stage::Bare);
    CHECK(forest.fields().biomass[cell] >= 0.4F);
    CHECK(forest.stage(middle() + 1) == Stage::Mature);
    // What's left is too little to burn again.
    CHECK_FALSE(forest.ignite(cell));

    const Weather good = good_year(forest.grid().size());
    // Grown back most of the way over the biome's 60 years.
    std::vector<Stage> stages{Stage::Bare};
    int mature = 0;
    for (int year = 1; year <= 62 && mature == 0; ++year) {
        forest.advance_to(forest.time() + days{365}, good.conditions(), jobs);
        if (forest.stage(cell) != stages.back()) {
            stages.push_back(forest.stage(cell));
        }
        mature = forest.stage(cell) == Stage::Mature ? year : 0;
    }
    CHECK(stages ==
          std::vector{Stage::Bare, Stage::Grass, Stage::Shrub, Stage::Young, Stage::Mature});
    CHECK(mature >= 58);
}

TEST_CASE("wet ground and water don't burn", "[sim][forest]") {
    Forest forest{config(), soil(), start};
    JobSystem jobs{1};
    const Weather wet{forest.grid().size(), 15.0F, 6.0F};
    forest.advance_to(forest.next_step(), wet.conditions(), jobs);
    CHECK(forest.flammability(2) == 0.0);
    CHECK_FALSE(forest.ignite(2));
    const Weather dry = drought(forest.grid().size());
    forest.advance_to(forest.next_step(), dry.conditions(), jobs);
    CHECK_FALSE(forest.ignite(0));
    CHECK(forest.ignite(2));
}

TEST_CASE("fire spreads further downwind than upwind", "[sim][forest]") {
    Forest forest{config(), woods(), start};
    JobSystem jobs{2};
    Weather windy = drought(forest.grid().size());
    // Blowing east everywhere, as near the middle of a face each cell's east lines up.
    std::ranges::fill(windy.wind, hog::Vec2{10.0F, 0.0F});
    forest.advance_to(forest.next_step(), windy.conditions(), jobs);
    const std::uint32_t origin = middle();
    REQUIRE(forest.ignite(origin));
    forest.advance_to(forest.time() + days{4}, windy.conditions(), jobs);

    int east = 0;
    int west = 0;
    for (std::uint32_t cell = 0; cell < forest.grid().size(); ++cell) {
        if (forest.fields().fire[cell] > 0.0F || forest.stage(cell) != Stage::Mature) {
            const double offset = east_of(forest.grid(), origin, cell);
            east += offset > 0.0 ? 1 : 0;
            west += offset < 0.0 ? 1 : 0;
        }
    }
    CHECK(east >= 3);
    CHECK(east > 3 * west);
    CHECK(forest.drain_fires().alight > 4.0 * forest.area(origin));
}

TEST_CASE("fire stops at water and burns itself out", "[sim][forest]") {
    // Forest on the first face only; ocean everywhere else.
    std::vector<Soil> island = woods();
    constexpr std::uint32_t face = 1U << (2U * static_cast<unsigned>(level));
    for (std::uint32_t cell = face; cell < island.size(); ++cell) {
        island[cell].biome = Biome::Ocean;
    }
    Forest forest{config(), island, start};
    JobSystem jobs{2};
    const Weather dry = drought(forest.grid().size());
    forest.advance_to(forest.next_step(), dry.conditions(), jobs);
    REQUIRE(forest.ignite(middle()));
    std::uint32_t most = 0;
    for (int day = 0; day < 120 && forest.burning() > 0; ++day) {
        forest.advance_to(forest.time() + days{1}, dry.conditions(), jobs);
        most = std::max(most, forest.burning());
        for (std::uint32_t cell = face; cell < island.size(); ++cell) {
            REQUIRE(forest.fields().fire[cell] == 0.0F);
        }
    }
    CHECK(most > 10);
    CHECK(forest.burning() == 0);
    std::uint32_t burnt = 0;
    for (std::uint32_t cell = 0; cell < face; ++cell) {
        burnt += forest.stage(cell) == Stage::Mature ? 0U : 1U;
    }
    CHECK(burnt > face / 2);
}

TEST_CASE("rain puts fires out", "[sim][forest]") {
    ForestConfig settings = config();
    settings.vegetation.fire.spread = 0.0;
    settings.vegetation.fire.burn_days = 10.0;
    Forest forest{settings, woods(), start};
    JobSystem jobs{1};
    Weather weather = drought(forest.grid().size());
    forest.advance_to(forest.next_step(), weather.conditions(), jobs);
    REQUIRE(forest.ignite(middle()));
    forest.advance_to(forest.time() + days{2}, weather.conditions(), jobs);
    REQUIRE(forest.burning() == 1);
    std::ranges::fill(weather.rain, 30.0F);
    forest.advance_to(forest.time() + days{1}, weather.conditions(), jobs);
    CHECK(forest.burning() == 0);
    CHECK(forest.stage(middle()) != Stage::Bare);
}

TEST_CASE("lightning lights dry plants under thunderstorms", "[sim][forest]") {
    ForestConfig settings = config();
    settings.vegetation.fire.lightning = 0.02;
    settings.vegetation.fire.spread = 0.0;
    JobSystem jobs{2};
    const auto fires = [&](float storm, float recent_rain, std::uint64_t seed) {
        settings.seed = seed;
        Forest forest{settings, woods(), start};
        Weather weather{forest.grid().size(), 30.0F, recent_rain};
        std::ranges::fill(weather.storm, storm);
        forest.advance_to(start + days{10}, weather.conditions(), jobs);
        return std::pair{forest.drain_fires().started, forest.fields()};
    };
    const auto [stormy, burnt] = fires(1.0F, 0.1F, 1);
    CHECK(stormy > 5);
    CHECK(stormy < 100);
    CHECK(fires(0.0F, 0.1F, 1).first == 0);
    CHECK(fires(1.0F, 8.0F, 1).first == 0);
    // The same strikes every time, and others for another seed.
    CHECK(fires(1.0F, 0.1F, 1).second == burnt);
    CHECK(fires(1.0F, 0.1F, 2).second != burnt);
}

TEST_CASE("fires spread the same on any number of threads", "[sim][forest]") {
    const auto run = [](std::uint32_t threads) {
        ForestConfig settings = config();
        settings.step = hours{1};
        settings.vegetation.fire.lightning = 0.01;
        Forest forest{settings, woods(), start};
        JobSystem jobs{threads};
        Weather weather = drought(forest.grid().size());
        std::ranges::fill(weather.storm, 0.5F);
        std::ranges::fill(weather.wind, hog::Vec2{3.0F, -2.0F});
        forest.advance_to(start + days{20}, weather.conditions(), jobs);
        return forest.fields();
    };
    const hog::sim::ForestFields one = run(1);
    CHECK(std::ranges::count_if(one.biomass, [](float biomass) { return biomass < 10.0F; }) > 0);
    CHECK(run(4) == one);
}

TEST_CASE("a forest saves and loads, fires and all", "[sim][forest]") {
    Forest saved{config(), woods(), start};
    JobSystem jobs{1};
    const Weather dry = drought(saved.grid().size());
    saved.advance_to(saved.next_step(), dry.conditions(), jobs);
    REQUIRE(saved.ignite(middle()));
    saved.advance_to(saved.time() + days{1}, dry.conditions(), jobs);
    REQUIRE(saved.burning() > 0);
    const auto bytes = saved.save();

    Forest loaded{config(), woods(), start + days{400}};
    REQUIRE(loaded.load(Forest::save_version, bytes));
    CHECK(loaded.time() == saved.time());
    CHECK(loaded.fields() == saved.fields());
    saved.advance_to(saved.time() + days{5}, dry.conditions(), jobs);
    loaded.advance_to(loaded.time() + days{5}, dry.conditions(), jobs);
    CHECK(loaded.fields() == saved.fields());

    ForestConfig finer_config = config();
    finer_config.level = level + 1;
    Forest finer{finer_config, std::vector<Soil>(hog::sim::CellGrid{level + 1}.size()), start};
    const auto other_level = finer.load(Forest::save_version, bytes);
    REQUIRE_FALSE(other_level);
    CHECK(other_level.error().code == hog::ErrorCode::InvalidArgument);
    const auto newer = loaded.load(Forest::save_version + 1, bytes);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == hog::ErrorCode::Unsupported);

    const auto before = loaded.fields();
    const std::span<const std::byte> all{bytes};
    CHECK_FALSE(loaded.load(Forest::save_version, all.first(all.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(loaded.load(Forest::save_version, longer));
    CHECK(loaded.fields() == before);
}
