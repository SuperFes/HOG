#include <hog/planet/elevation.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/tile.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using hog::planet::CellId;
using hog::planet::Elevation;
using hog::planet::fixed_one;

namespace {

std::vector<CellId> cells_at(int level) {
    std::vector<CellId> cells;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const auto tile = hog::planet::sample_tile(CellId::from_face(face), level,
                                                   [](CellId cell) { return cell; });
        cells.insert(cells.end(), tile.begin(), tile.end());
    }
    return cells;
}

std::int64_t metres(std::int64_t value) {
    return value * fixed_one;
}

} // namespace

TEST_CASE("land stays above sea level and the ocean floor below it", "[planet][elevation]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
    const Elevation terrain{seed, 0.7};
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.continents().is_land(cell)) {
            REQUIRE(elevation >= 0);
            REQUIRE(elevation <= metres(Elevation::lowland_height + Elevation::mountain_height));
        } else {
            REQUIRE(elevation <= 0);
            REQUIRE(elevation >= metres(-Elevation::ocean_depth));
            REQUIRE(terrain.mountain_strength(cell) == 0);
        }
    }
}

TEST_CASE("mountains rise on part of the land and the ocean is deep", "[planet][elevation]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Elevation terrain{seed, 0.7};
    std::size_t land = 0;
    std::size_t high = 0;
    std::int64_t highest = 0;
    std::vector<std::int64_t> depths;
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.continents().is_land(cell)) {
            ++land;
            if (elevation > metres(1'000)) {
                ++high;
            }
            highest = std::max(highest, elevation);
        } else {
            depths.push_back(-elevation);
        }
    }
    CHECK(high * 100 > land);
    CHECK(high * 4 < land);
    CHECK(highest > metres(2'000));
    std::ranges::nth_element(depths,
                             depths.begin() + static_cast<std::ptrdiff_t>(depths.size() / 2));
    CHECK(depths[depths.size() / 2] > metres(2'500));
}

TEST_CASE("coasts stay low", "[planet][elevation]") {
    const Elevation terrain{4, 0.7};
    const auto& continents = terrain.continents();
    std::int64_t highest_coast = 0;
    for (const CellId cell :
         hog::planet::sample_tile(CellId::from_face(4), 8, [](CellId cell) { return cell; })) {
        if (!continents.is_land(cell)) {
            continue;
        }
        const auto neighbours = hog::planet::edge_neighbors(cell);
        if (std::ranges::any_of(neighbours,
                                [&](CellId next) { return !continents.is_land(next); })) {
            highest_coast = std::max(highest_coast, terrain.elevation(cell));
        }
    }
    CHECK(highest_coast > 0);
    CHECK(highest_coast < metres(500));
}

TEST_CASE("elevation changes smoothly between neighbours", "[planet][elevation]") {
    // Steps shrink with the cells; a discontinuity would not.
    const Elevation terrain{2, 0.7};
    constexpr int level = 16;
    std::mt19937_64 rng{11};
    std::uniform_int_distribution<std::uint32_t> coordinate{0, (1U << level) - 1};
    std::uniform_int_distribution<int> face{0, hog::planet::face_count - 1};
    std::int64_t largest_step = 0;
    for (int sample = 0; sample < 10'000; ++sample) {
        const CellId cell =
            CellId::from_face_ij(face(rng), coordinate(rng), coordinate(rng), level);
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            largest_step =
                std::max(largest_step, std::abs(terrain.elevation(cell) - terrain.elevation(next)));
        }
    }
    // Cells here are about 3 m across.
    CHECK(largest_step < metres(10));
}

TEST_CASE("elevation depends on the seed", "[planet][elevation]") {
    const CellId cell = CellId::from_face_ij(2, 100, 200, 10);
    const Elevation first{1, 0.7};
    CHECK(Elevation{1, 0.7}.elevation(cell) == first.elevation(cell));
    CHECK(Elevation{2, 0.7}.elevation(cell) != first.elevation(cell));
}
