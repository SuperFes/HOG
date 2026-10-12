#include <hog/jobs/job_system.hpp>
#include <hog/sim/forest.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstdint>
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

    Weather(std::size_t size, float degrees, float rain)
        : temperature(size, degrees), precipitation(size, rain) {}

    [[nodiscard]] ForestConditions conditions() const {
        return ForestConditions{.temperature = temperature, .precipitation = precipitation};
    }
};

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
    CHECK(stage_of(2.0, 20.0) == Stage::Grass);
    CHECK(stage_of(6.0, 20.0) == Stage::Shrub);
    CHECK(stage_of(12.0, 20.0) == Stage::Young);
    CHECK(stage_of(19.0, 20.0) == Stage::Mature);
    CHECK(hog::sim::to_string(Stage::Shrub) == "shrub");
}
