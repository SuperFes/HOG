#include <hog/core/hash.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/tile.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::planet::CellId;
using hog::planet::Elevation;
using hog::planet::fixed_one;

namespace {

hog::planet::PlanetConfig planet(std::uint64_t seed) {
    return {.seed = seed, .ocean_fraction = 0.7, .radius = 115'000.0};
}

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
    const Elevation terrain{planet(seed)};
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.is_land(cell)) {
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
    const Elevation terrain{planet(seed)};
    std::size_t land = 0;
    std::size_t high = 0;
    std::int64_t highest = 0;
    std::vector<std::int64_t> depths;
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.is_land(cell)) {
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
    const Elevation terrain{planet(4)};
    std::int64_t highest_coast = 0;
    for (const CellId cell :
         hog::planet::sample_tile(CellId::from_face(4), 8, [](CellId cell) { return cell; })) {
        if (!terrain.is_land(cell)) {
            continue;
        }
        const auto neighbours = hog::planet::edge_neighbors(cell);
        if (std::ranges::any_of(neighbours, [&](CellId next) { return !terrain.is_land(next); })) {
            highest_coast = std::max(highest_coast, terrain.elevation(cell));
        }
    }
    CHECK(highest_coast > 0);
    CHECK(highest_coast < metres(500));
}

TEST_CASE("elevation changes smoothly between neighbours", "[planet][elevation]") {
    // Steps shrink with the cells; a discontinuity would not.
    const Elevation terrain{planet(2)};
    constexpr int level = 16;
    constexpr std::uint64_t side = std::uint64_t{1} << level;
    std::int64_t largest_step = 0;
    for (std::uint64_t sample = 0; sample < 10'000; ++sample) {
        const CellId cell = CellId::from_face_ij(
            static_cast<int>(hog::hash_values(sample, 0) % hog::planet::face_count),
            static_cast<std::uint32_t>(hog::hash_values(sample, 1) % side),
            static_cast<std::uint32_t>(hog::hash_values(sample, 2) % side), level);
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
    const Elevation first{planet(1)};
    CHECK(Elevation{planet(1)}.elevation(cell) == first.elevation(cell));
    CHECK(Elevation{planet(2)}.elevation(cell) != first.elevation(cell));
}

