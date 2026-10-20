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
    std::vector<Soil> soil(grid.size(), Soil{.biome = Biome::TemperateForest});
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
    std::vector<float> soil;
    std::vector<float> snow;
    std::vector<float> rain;
    std::vector<float> storm;
    std::vector<hog::Vec2> wind;
    // At its usual level unless set.
    std::vector<double> groundwater;
    std::vector<double> usual_groundwater;

    Weather(std::size_t size, float degrees, float fullness)
        : temperature(size, degrees), soil(size, fullness), snow(size), rain(size), storm(size),
          wind(size, hog::Vec2{0.0F}), groundwater(size, 100.0), usual_groundwater(size, 100.0) {}

    [[nodiscard]] ForestConditions conditions() const {
        return ForestConditions{.temperature = temperature,
                                .soil = soil,
                                .snow = snow,
                                .rain = rain,
                                .rain_days = 1.0,
                                .storm = storm,
                                .wind = wind,
                                .groundwater = groundwater,
                                .usual_groundwater = usual_groundwater};
    }
};

// Hot, with no rain for weeks: dry enough for the forest to burn.
Weather drought(std::size_t size) {
    return Weather{size, 30.0F, 0.05F};
}

// Warm and wet enough to grow at full speed.
Weather good_year(std::size_t size) {
    return Weather{size, 20.0F, 0.8F};
}

// Forest everywhere.
std::vector<Soil> woods() {
    return std::vector<Soil>(hog::sim::CellGrid{level}.size(),
                             Soil{.biome = Biome::TemperateForest});
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
    const Weather summer{forest.grid().size(), 20.0F, 0.8F};
    CHECK(forest.advance_to(start + days{365}, summer.conditions(), jobs) == 365);
    CHECK(forest.time() == start + days{365});
    for (std::uint32_t cell = 0; cell < forest.grid().size(); ++cell) {
        CHECK_THAT(forest.fields().biomass[cell], WithinRel(forest.capacity(cell), 1e-5));
    }
    CHECK(forest.advance_to(start + days{365} + hours{23}, summer.conditions(), jobs) == 0);
}

TEST_CASE("the ground is as dry as its soil is empty, and snow keeps it damp", "[sim][forest]") {
    using hog::sim::dryness;
    CHECK_THAT(dryness(0.0, 0.0), WithinAbs(1.0, 1e-9));
    CHECK_THAT(dryness(0.3, 0.0), WithinAbs(0.7, 1e-9));
    CHECK_THAT(dryness(1.0, 0.0), WithinAbs(0.0, 1e-9));
    CHECK(dryness(0.0, 5.0) == 0.0);

    Forest forest{config(), soil(), start};
    JobSystem jobs{1};
    Weather drought{forest.grid().size(), 30.0F, 0.2F};
    forest.advance_to(forest.next_step(), drought.conditions(), jobs);
    CHECK_THAT(forest.fields().dryness[2], WithinAbs(0.8, 1e-6));
    std::ranges::fill(drought.snow, 20.0F);
    forest.advance_to(forest.next_step(), drought.conditions(), jobs);
    CHECK(forest.fields().dryness[2] == 0.0F);
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

TEST_CASE("a fire leaves its dead standing charred, and what grows back is young for generations",
          "[sim][forest]") {
    ForestConfig settings = config();
    settings.vegetation.fire.spread = 0.0;
    Forest forest{settings, woods(), start};
    JobSystem jobs{2};
    const std::uint32_t cell = middle();
    const std::uint32_t untouched = middle() + 1;
    const Weather dry = drought(forest.grid().size());
    forest.advance_to(forest.next_step(), dry.conditions(), jobs);
    REQUIRE(forest.ignite(cell));
    forest.advance_to(forest.time() + days{3}, dry.conditions(), jobs);
    REQUIRE(forest.burning() == 0);
    const hog::sim::ForestFields& fields = forest.fields();
    // Burned down to its seeds, the rest standing dead and black.
    CHECK_THAT(forest.living(cell), WithinAbs(0.02, 1e-3));
    CHECK_THAT(static_cast<double>(fields.dead[cell]), WithinAbs(0.98, 0.01));
    CHECK_THAT(static_cast<double>(fields.charred[cell]), WithinAbs(1.0, 1e-5));
    CHECK(static_cast<double>(fields.established[cell]) <= forest.living(cell) + 1e-6);
    CHECK(fields.failing[cell] == 0.0F);
    // Its neighbour untouched.
    CHECK(forest.living(untouched) == 1.0);
    CHECK(fields.established[untouched] == 1.0F);
    CHECK(fields.dead[untouched] == 0.0F);

    const Weather good = good_year(forest.grid().size());
    forest.advance_to(forest.time() + days{365 * 10}, good.conditions(), jobs);
    const double living = forest.living(cell);
    // A decade on, the snags thinning, the regrowth all young, never more alive and dead than
    // the ground holds.
    CHECK(fields.dead[cell] < 0.6F);
    CHECK(fields.dead[cell] > 0.1F);
    CHECK(static_cast<double>(fields.dead[cell]) + living <= 1.0 + 1e-6);
    CHECK(living > 0.05);
    CHECK(static_cast<double>(fields.established[cell]) < 0.5 * living);
    forest.advance_to(forest.time() + days{365 * 140}, good.conditions(), jobs);
    // Generations on, grown back, the dead all fallen, the most of it old growth again.
    CHECK(forest.living(cell) > 0.95);
    CHECK(fields.dead[cell] < 0.01F);
    CHECK(static_cast<double>(fields.established[cell]) > 0.7 * forest.living(cell));
}

TEST_CASE("a drought kills back what the ground water can't keep, the failing first",
          "[sim][forest]") {
    Forest forest{config(), woods(), start};
    JobSystem jobs{2};
    const std::uint32_t cell = middle();
    Weather weather = good_year(forest.grid().size());
    const hog::sim::ForestFields& fields = forest.fields();
    // The first year, the plants learn the dry season they're used to, and the next costs them
    // nothing.
    std::ranges::fill(weather.groundwater, 80.0);
    forest.advance_to(start + days{366}, weather.conditions(), jobs);
    CHECK(forest.time() >= forest.learning_until());
    CHECK_THAT(static_cast<double>(fields.accustomed[cell]), WithinAbs(0.2, 0.02));
    forest.advance_to(forest.time() + days{365}, weather.conditions(), jobs);
    CHECK(forest.living(cell) == 1.0);
    CHECK(fields.failing[cell] == 0.0F);

    // Years with the ground water well down: dying back toward what it keeps.
    std::ranges::fill(weather.groundwater, 30.0);
    forest.advance_to(forest.time() + days{120}, weather.conditions(), jobs);
    CHECK(fields.shortfall[cell] > 0.4F);
    CHECK(fields.failing[cell] > 0.0F);
    forest.advance_to(forest.time() + days{365 * 5}, weather.conditions(), jobs);
    const double keeps = forest.sustainable(cell);
    const double bearable = static_cast<double>(fields.accustomed[cell]) + 0.1;
    CHECK_THAT(static_cast<double>(fields.shortfall[cell]), WithinAbs(0.7, 0.01));
    // Slowly growing used to it.
    CHECK(fields.accustomed[cell] > 0.25F);
    CHECK(fields.accustomed[cell] < 0.4F);
    CHECK_THAT(keeps, WithinAbs(1.0 - (0.6 * (0.7 - bearable) / (1.0 - bearable)), 0.01));
    CHECK_THAT(forest.living(cell), WithinAbs(keeps, 0.05));
    CHECK(fields.dead[cell] > 0.1F);
    CHECK(fields.charred[cell] == 0.0F);
    CHECK(static_cast<double>(fields.established[cell]) <= forest.living(cell) + 1e-6);
    CHECK(fields.failing[cell] < 0.05F);

    // The water back, the woods grow back, nothing failing.
    std::ranges::fill(weather.groundwater, 100.0);
    forest.advance_to(forest.time() + days{365 * 30}, weather.conditions(), jobs);
    CHECK(forest.living(cell) > 0.9);
    CHECK(fields.failing[cell] == 0.0F);
    CHECK(fields.shortfall[cell] < 0.01F);
    // With none known, nothing dies of drought.
    Forest unknown{config(), woods(), start};
    Weather unmeasured = good_year(unknown.grid().size());
    unmeasured.groundwater.clear();
    unmeasured.usual_groundwater.clear();
    unknown.advance_to(start + days{365}, unmeasured.conditions(), jobs);
    CHECK(unknown.living(cell) == 1.0);
}

TEST_CASE("a new forest loses nothing to drought while it learns what's usual", "[sim][forest]") {
    Forest forest{config(), woods(), start};
    JobSystem jobs{2};
    const std::uint32_t cell = middle();
    Weather weather = good_year(forest.grid().size());
    std::ranges::fill(weather.groundwater, 10.0);
    forest.advance_to(start + days{300}, weather.conditions(), jobs);
    CHECK(forest.fields().shortfall[cell] > 0.8F);
    CHECK(forest.sustainable(cell) == 1.0);
    CHECK(forest.living(cell) == 1.0);
    // Learned, a drought as bad as that one is usual.
    forest.advance_to(start + days{730}, weather.conditions(), jobs);
    CHECK(forest.living(cell) == 1.0);
    // Without learning, it's a drought from the first.
    ForestConfig hasty = config();
    hasty.vegetation.dying.learning = 0.0;
    Forest unready{hasty, woods(), start};
    unready.advance_to(start + days{300}, weather.conditions(), jobs);
    CHECK(unready.living(cell) < 0.9);
}

TEST_CASE("wet ground and water don't burn", "[sim][forest]") {
    Forest forest{config(), soil(), start};
    JobSystem jobs{1};
    const Weather wet{forest.grid().size(), 15.0F, 1.0F};
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
    // Fierce enough to spread across the whole face.
    ForestConfig settings = config();
    settings.vegetation.fire.spread = 6.0;
    Forest forest{settings, island, start};
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
    const auto fires = [&](float storm, float soil, std::uint64_t seed) {
        settings.seed = seed;
        Forest forest{settings, woods(), start};
        Weather weather{forest.grid().size(), 30.0F, soil};
        std::ranges::fill(weather.storm, storm);
        forest.advance_to(start + days{10}, weather.conditions(), jobs);
        return std::pair{forest.drain_fires().started, forest.fields()};
    };
    const auto [stormy, burnt] = fires(1.0F, 0.05F, 1);
    CHECK(stormy > 5);
    CHECK(stormy < 100);
    CHECK(fires(0.0F, 0.05F, 1).first == 0);
    CHECK(fires(1.0F, 1.0F, 1).first == 0);
    // The same strikes every time, and others for another seed.
    CHECK(fires(1.0F, 0.05F, 1).second == burnt);
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
    CHECK(loaded.fields().dead[middle()] > 0.0F);
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

    // The first version, without the fates: the living counted old growth, nothing dead.
    const std::size_t cells = saved.grid().size();
    const std::span<const std::byte> first{bytes.data(), 1 + 8 + (3 * 4 * cells)};
    Forest older{config(), woods(), start};
    REQUIRE(older.load(1, first));
    Forest current{config(), woods(), start};
    REQUIRE(current.load(Forest::save_version, bytes));
    CHECK(older.fields().biomass == current.fields().biomass);
    for (std::uint32_t cell = 0; cell < cells; ++cell) {
        CHECK_THAT(static_cast<double>(older.fields().established[cell]),
                   WithinAbs(older.living(cell), 1e-6));
        CHECK(older.fields().dead[cell] == 0.0F);
    }
    // The second, without what the plants are used to: they learn it afresh.
    const std::span<const std::byte> second{bytes.data(), bytes.size() - (4 * cells) - 8};
    Forest unlearned{config(), woods(), start};
    REQUIRE(unlearned.load(2, second));
    CHECK(unlearned.fields().accustomed == current.fields().shortfall);
    CHECK(unlearned.learning_until() > unlearned.time());

    const auto before = loaded.fields();
    const std::span<const std::byte> all{bytes};
    CHECK_FALSE(loaded.load(Forest::save_version, all.first(all.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(loaded.load(Forest::save_version, longer));
    CHECK(loaded.fields() == before);
}
