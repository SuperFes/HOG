#include <hog/planet/archipelago.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_set>
#include <vector>

using hog::planet::Archipelago;
using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::Elevation;
using hog::planet::fixed_one;

namespace {

constexpr double radius = 115'000.0;
// Cells about 90 m across on a planet of this radius.
constexpr int level = 11;

hog::planet::PlanetConfig planet(std::uint64_t seed, double planet_radius = radius) {
    return {.seed = seed, .ocean_fraction = 0.7, .radius = planet_radius};
}

struct CellHash {
    std::size_t operator()(CellId cell) const noexcept {
        return std::hash<std::uint64_t>{}(cell.raw());
    }
};

// The connected land around an island's centre, or everything found before giving up.
std::vector<CellId> island_cells(const Elevation& terrain, hog::DVec3 center, int cell_level) {
    constexpr std::size_t limit = 100'000;
    const CellId start = hog::planet::cell_at(center, cell_level);
    std::vector<CellId> cells;
    if (!terrain.is_land(start)) {
        return cells;
    }
    std::unordered_set<CellId, CellHash> seen{start};
    cells.push_back(start);
    for (std::size_t next = 0; next < cells.size() && cells.size() < limit; ++next) {
        for (const CellId neighbour : hog::planet::edge_neighbors(cells[next])) {
            if (!seen.contains(neighbour) && terrain.is_land(neighbour)) {
                seen.insert(neighbour);
                cells.push_back(neighbour);
            }
        }
    }
    return cells;
}

std::vector<CellId> coast(const Elevation& terrain, const std::vector<CellId>& cells) {
    std::vector<CellId> shore;
    for (const CellId cell : cells) {
        const auto neighbours = hog::planet::edge_neighbors(cell);
        if (std::ranges::any_of(neighbours, [&](CellId next) { return !terrain.is_land(next); })) {
            shore.push_back(cell);
        }
    }
    return shore;
}

double square_kilometres(const std::vector<CellId>& cells, double planet_radius) {
    double area = 0.0;
    for (const CellId cell : cells) {
        area += hog::planet::cell_area(cell);
    }
    return area * planet_radius * planet_radius / 1e6;
}

} // namespace

TEST_CASE("the starting islands are separate and 10 to 60 square kilometres",
          "[planet][archipelago]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Elevation terrain{planet(seed)};
    const auto islands = terrain.archipelago().islands();
    REQUIRE(islands.size() >= Archipelago::min_islands);
    REQUIRE(islands.size() <= Archipelago::max_islands);
    for (const auto& island : islands) {
        const auto cells = island_cells(terrain, island.center, level);
        REQUIRE_FALSE(cells.empty());
        const double area = square_kilometres(cells, radius);
        INFO("area " << area);
        CHECK(area > 10.0);
        CHECK(area < 60.0);
        for (const CellId cell : cells) {
            // Never joined to a continent, never above lowland hills.
            REQUIRE_FALSE(terrain.continents().is_land(cell));
            REQUIRE(terrain.elevation(cell) <= Elevation::lowland_height * fixed_one);
        }
        for (const auto& other : islands) {
            if (&other != &island) {
                REQUIRE(std::ranges::find(cells, hog::planet::cell_at(other.center, level)) ==
                        cells.end());
            }
        }
    }
}

TEST_CASE("the starting islands are irregular rather than round", "[planet][archipelago]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Elevation terrain{planet(seed)};
    std::vector<double> ratios;
    for (const auto& island : terrain.archipelago().islands()) {
        CHECK(island.elongation >= 1.0);
        CHECK(island.elongation <= Archipelago::max_elongation);
        CHECK(std::abs(glm::length(island.axis) - 1.0) < 1e-12);
        CHECK(std::abs(glm::dot(island.axis, island.center)) < 1e-12);
        // How much farther the farthest stretch of coast is from the centre than the nearest.
        double nearest = 1e300;
        double farthest = 0.0;
        for (const CellId cell : coast(terrain, island_cells(terrain, island.center, level))) {
            const double distance = glm::length(hog::planet::cell_center(cell) - island.center);
            nearest = std::min(nearest, distance);
            farthest = std::max(farthest, distance);
        }
        ratios.push_back(farthest / nearest);
    }
    std::ranges::sort(ratios);
    INFO("least " << ratios.front() << ", median " << ratios[ratios.size() / 2]);
    CHECK(ratios.front() > 1.2);
    CHECK(ratios[ratios.size() / 2] > 1.5);
}

TEST_CASE("the starting islands are within sailing distance of each other",
          "[planet][archipelago]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Elevation terrain{planet(seed)};
    const auto islands = terrain.archipelago().islands();
    std::vector<std::vector<hog::DVec3>> shores;
    for (const auto& island : islands) {
        std::vector<hog::DVec3> shore;
        for (const CellId cell : coast(terrain, island_cells(terrain, island.center, level))) {
            shore.push_back(hog::planet::cell_center(cell));
        }
        shores.push_back(std::move(shore));
    }
    const auto gap = [&](std::size_t a, std::size_t b) {
        double closest = 1e300;
        for (const hog::DVec3 p : shores[a]) {
            for (const hog::DVec3 q : shores[b]) {
                closest = std::min(closest, glm::length(p - q) * radius);
            }
        }
        return closest;
    };
    // Every island reachable by short crossings of open water.
    constexpr double sailing_distance = 8'000.0;
    std::vector<bool> reached(islands.size(), false);
    std::vector<std::size_t> frontier{0};
    reached[0] = true;
    while (!frontier.empty()) {
        const std::size_t from = frontier.back();
        frontier.pop_back();
        for (std::size_t to = 0; to < islands.size(); ++to) {
            if (!reached[to] && gap(from, to) < sailing_distance) {
                reached[to] = true;
                frontier.push_back(to);
            }
        }
    }
    CHECK(std::ranges::all_of(reached, [](bool r) { return r; }));
}

TEST_CASE("the starting islands sit in temperate open ocean", "[planet][archipelago]") {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        INFO("seed " << seed);
        const Elevation terrain{planet(seed)};
        const hog::DVec3 center = terrain.archipelago().center();
        const double latitude = std::asin(std::abs(center.z)) * 180.0 / std::numbers::pi;
        CHECK(latitude > 35.0);
        CHECK(latitude < 50.0);
        CHECK(terrain.continents().height(hog::planet::cell_at(center)) < 0);
    }
}

TEST_CASE("the starting islands are mostly habitable", "[planet][archipelago]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const hog::planet::Climate climate{planet(seed)};
    double land = 0.0;
    double habitable = 0.0;
    for (const auto& island : climate.terrain().archipelago().islands()) {
        for (const CellId cell : island_cells(climate.terrain(), island.center, level)) {
            const double area = hog::planet::cell_area(cell);
            land += area;
            switch (climate.biome(cell)) {
            case Biome::Desert:
            case Biome::Ice:
            case Biome::Tundra:
            case Biome::Alpine:
                break;
            default:
                habitable += area;
            }
        }
    }
    CHECK(habitable > land * 0.8);
}

TEST_CASE("the starting islands keep their size on an Earth-sized planet",
          "[planet][archipelago]") {
    constexpr double earth = 6'371'000.0;
    // About 76 m cells at this radius.
    constexpr int earth_level = 17;
    const Elevation terrain{planet(1, earth)};
    for (const auto& island : terrain.archipelago().islands()) {
        const double area =
            square_kilometres(island_cells(terrain, island.center, earth_level), earth);
        CHECK(area > 10.0);
        CHECK(area < 60.0);
    }
}

TEST_CASE("the starting islands rise smoothly from the sea floor", "[planet][archipelago]") {
    // Cells here are about 2.7 m across; flanks of 15 degrees or so climb under 1 m per cell,
    // steepening off rugged coasts but never into cliffs or seams.
    constexpr int fine_level = 16;
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Elevation terrain{planet(seed)};
    std::vector<std::int64_t> steps;
    for (const auto& island : terrain.archipelago().islands()) {
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(island.center);
        const double diagonal = std::sqrt(0.5);
        for (const hog::DVec3 direction : {frame.east, (frame.east + frame.north) * diagonal,
                                           frame.north, (frame.north - frame.east) * diagonal,
                                           -frame.east, (-frame.east - frame.north) * diagonal,
                                           -frame.north, (frame.east - frame.north) * diagonal}) {
            // A line of cells from the centre out across the coast and flank.
            const double longest = island.radius * std::sqrt(island.elongation);
            const auto count = static_cast<int>(2.5 * longest / 2.0);
            for (int step = 0; step < count; ++step) {
                const double metres = 2.0 * step;
                const CellId cell = hog::planet::cell_at(
                    glm::normalize(island.center + (direction * (metres / radius))), fine_level);
                for (const CellId next : hog::planet::edge_neighbors(cell)) {
                    steps.push_back(std::abs(terrain.elevation(cell) - terrain.elevation(next)));
                }
            }
        }
    }
    std::ranges::sort(steps);
    CHECK(steps[steps.size() * 95 / 100] < 3 * fixed_one / 2);
    CHECK(steps.back() < 6 * fixed_one);
}

TEST_CASE("the starting islands depend on the seed", "[planet][archipelago]") {
    const hog::DVec3 first = Elevation{planet(1)}.archipelago().center();
    CHECK(Elevation{planet(1)}.archipelago().center() == first);
    CHECK(Elevation{planet(2)}.archipelago().center() != first);
}

TEST_CASE("archipelago preconditions", "[planet][archipelago]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(Elevation{planet(1, Archipelago::clearance)}, hog::testing::AssertionFailed);
}
