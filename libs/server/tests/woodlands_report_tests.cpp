#include <hog/jobs/job_system.hpp>
#include <hog/server/woodlands_report.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using hog::planet::Biome;
using hog::protocol::CellWoodland;
using hog::server::woodlands_report;
using hog::sim::Forest;
using hog::sim::ForestConfig;
using hog::sim::Soil;

namespace {

constexpr hog::GameTime start{sys_days{2026y / June / 1}.time_since_epoch()};

ForestConfig config(int level) {
    ForestConfig out{.level = level, .step = hours{24}};
    out.vegetation.growth.capacity[static_cast<std::size_t>(Biome::TemperateForest)] = 20.0;
    out.vegetation.growth.capacity[static_cast<std::size_t>(Biome::Grassland)] = 0.5;
    out.vegetation.fire.spread = 0.0;
    return out;
}

// Hot, still and parched everywhere, the ground water at its usual level.
struct Parched {
    std::vector<float> temperature;
    std::vector<float> soil;
    std::vector<float> zero;
    std::vector<hog::Vec2> wind;
    std::vector<double> groundwater;

    explicit Parched(std::size_t size)
        : temperature(size, 30.0F), soil(size, 0.05F), zero(size), wind(size, hog::Vec2{0.0F}),
          groundwater(size, 100.0) {}

    [[nodiscard]] hog::sim::ForestConditions conditions() const {
        return {.temperature = temperature,
                .soil = soil,
                .snow = zero,
                .rain = zero,
                .rain_days = 1.0,
                .storm = zero,
                .wind = wind,
                .groundwater = groundwater,
                .usual_groundwater = groundwater};
    }
};

// Burns the cell down.
void burn(Forest& forest, std::uint32_t cell) {
    hog::jobs::JobSystem jobs{1};
    const Parched parched{forest.grid().size()};
    forest.advance_to(forest.next_step(), parched.conditions(), jobs);
    REQUIRE(forest.ignite(cell));
    forest.advance_to(forest.time() + days{3}, parched.conditions(), jobs);
    REQUIRE(forest.burning() == 0);
}

} // namespace

TEST_CASE("woods as coarse as may be sent go cell for cell, untouched where nothing grows",
          "[server][woodlands]") {
    std::vector<Soil> soil(hog::sim::CellGrid{3}.size(), Soil{.biome = Biome::TemperateForest});
    soil[0].biome = Biome::Ocean;
    Forest forest{config(3), soil, start};
    constexpr std::uint32_t burned = 36;
    burn(forest, burned);

    const hog::protocol::Woodlands woodlands = woodlands_report(forest);
    CHECK(woodlands.level == 3);
    REQUIRE(woodlands.cells.size() == forest.grid().size());
    const CellWoodland& black = woodlands.cells[burned];
    CHECK_THAT(black.living, WithinAbs(forest.living(burned), 1e-6));
    CHECK(black.established == forest.fields().established[burned]);
    CHECK(black.dead == forest.fields().dead[burned]);
    CHECK_THAT(black.charred, WithinAbs(1.0, 1e-5));
    CHECK(black.scar == forest.fields().scar[burned]);
    CHECK(black.scar > 0.9F);
    CHECK(woodlands.cells[0] == CellWoodland{});
    CHECK(woodlands.cells[burned + 1] == CellWoodland{});
}

TEST_CASE("finer woods are sent averaged over each cell sent, by what each holds",
          "[server][woodlands]") {
    // Under one cell sent, a forest burned beside grassland that holds a fortieth as much.
    const hog::sim::CellGrid grid{6};
    std::vector<Soil> soil(grid.size(), Soil{.biome = Biome::TemperateForest});
    const hog::planet::CellId sent = grid.cell(100).parent(5);
    std::vector<std::uint32_t> under;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (grid.cell(index).parent(5) == sent) {
            under.push_back(index);
        }
    }
    REQUIRE(under.size() == 4);
    soil[under[1]].biome = Biome::Grassland;
    Forest mixed{config(6), soil, start};
    burn(mixed, under[0]);

    const hog::protocol::Woodlands woodlands = woodlands_report(mixed);
    CHECK(woodlands.level == hog::protocol::most_weather_level);
    REQUIRE(woodlands.cells.size() == hog::protocol::weather_cells(5));
    const CellWoodland& cell = woodlands.cells[hog::sim::CellGrid{5}.index(sent)];
    double weights = 0.0;
    double living = 0.0;
    for (const std::uint32_t index : under) {
        const double weight = mixed.capacity(index) * grid.area(index);
        weights += weight;
        living += weight * mixed.living(index);
    }
    CHECK_THAT(cell.living, WithinAbs(living / weights, 1e-5));
    // The scar lies on the ground, so it's averaged by area alone: one of four burned.
    double scar = 0.0;
    double area = 0.0;
    for (const std::uint32_t index : under) {
        scar += grid.area(index) * static_cast<double>(mixed.fields().scar[index]);
        area += grid.area(index);
    }
    CHECK_THAT(cell.scar, WithinAbs(scar / area, 1e-5));
    CHECK(cell.scar > 0.2F);
    CHECK(cell.scar < 0.3F);
    CHECK(cell.living < 0.8F);
    CHECK(cell.dead > 0.2F);
    CHECK_THAT(cell.charred, WithinAbs(1.0, 1e-5));
}
