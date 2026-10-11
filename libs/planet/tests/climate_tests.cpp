#include <hog/core/hash.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::classify_biome;
using hog::planet::Climate;
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

constexpr std::int64_t celsius(std::int64_t degrees) {
    return degrees * fixed_one;
}

double sine_of_latitude(CellId cell) {
    return std::abs(hog::planet::cell_center(cell).z);
}

struct Mean {
    double sum = 0.0;
    double weight = 0.0;

    void add(double value, double area) {
        sum += value * area;
        weight += area;
    }
    [[nodiscard]] double value() const { return sum / weight; }
};

} // namespace

TEST_CASE("biomes follow temperature and moisture", "[planet][climate]") {
    static_assert(classify_biome(celsius(26), fixed_one) == Biome::TropicalRainforest);
    static_assert(classify_biome(celsius(26), fixed_one * 2 / 5) == Biome::Savanna);
    static_assert(classify_biome(celsius(26), 0) == Biome::Desert);
    static_assert(classify_biome(celsius(12), fixed_one) == Biome::TemperateForest);
    static_assert(classify_biome(celsius(12), fixed_one / 3) == Biome::Shrubland);
    static_assert(classify_biome(celsius(12), fixed_one / 5) == Biome::Grassland);
    static_assert(classify_biome(celsius(12), 0) == Biome::Desert);
    static_assert(classify_biome(celsius(0), fixed_one) == Biome::BorealForest);
    static_assert(classify_biome(celsius(0), 0) == Biome::Tundra);
    static_assert(classify_biome(celsius(-8), fixed_one) == Biome::Tundra);

    // Wetter never means a drier biome at the same temperature, and freezing is always ice.
    constexpr auto wetness_rank = [](Biome biome) {
        switch (biome) {
        case Biome::Desert:
        case Biome::Tundra:
            return 0;
        case Biome::Grassland:
            return 1;
        case Biome::Shrubland:
        case Biome::Savanna:
            return 2;
        default:
            return 3;
        }
    };
    for (std::int64_t temperature = celsius(-40); temperature <= celsius(40);
         temperature += fixed_one / 4) {
        int previous = 0;
        for (std::int64_t moisture = 0; moisture <= fixed_one; moisture += fixed_one / 64) {
            const Biome biome = classify_biome(temperature, moisture);
            REQUIRE(biome != Biome::Ocean);
            REQUIRE(biome != Biome::Beach);
            REQUIRE(biome != Biome::Alpine);
            REQUIRE((biome == Biome::Ice) == (temperature < celsius(-12)));
            REQUIRE(wetness_rank(biome) >= previous);
            previous = wetness_rank(biome);
        }
    }
}

TEST_CASE("every biome has a distinct name", "[planet][climate]") {
    std::set<std::string_view> names;
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        names.insert(to_string(static_cast<Biome>(biome)));
    }
    CHECK(names.size() == hog::planet::biome_count);
    CHECK_FALSE(names.contains("unknown"));
}

TEST_CASE("the poles are colder than the equator", "[planet][climate]") {
    const Climate climate{planet(1)};
    std::int64_t warmest_pole = celsius(-100);
    std::int64_t coldest_equator = celsius(100);
    for (const CellId cell : cells_at(6)) {
        const double sine = sine_of_latitude(cell);
        if (sine > 0.95) {
            warmest_pole = std::max(warmest_pole, climate.temperature(cell));
        } else if (sine < 0.1 && !climate.terrain().is_land(cell)) {
            coldest_equator = std::min(coldest_equator, climate.temperature(cell));
        }
    }
    CHECK(warmest_pole < celsius(-10));
    CHECK(coldest_equator > celsius(20));
}

TEST_CASE("temperature falls with height", "[planet][climate]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
    const Climate climate{planet(seed)};
    Mean lowland;
    Mean highland;
    for (const CellId cell : cells_at(6)) {
        if (sine_of_latitude(cell) > 0.5) {
            continue;
        }
        const std::int64_t elevation = climate.terrain().elevation(cell);
        const auto temperature = static_cast<double>(climate.temperature(cell));
        if (elevation > 0 && elevation < 500 * fixed_one) {
            lowland.add(temperature, 1.0);
        } else if (elevation > 2'000 * fixed_one) {
            highland.add(temperature, 1.0);
        }
    }
    REQUIRE(highland.weight > 0.0);
    CHECK(lowland.value() - highland.value() > static_cast<double>(celsius(8)));
}

TEST_CASE("moisture follows the circulation bands", "[planet][climate]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
    const Climate climate{planet(seed)};
    // Over the ocean, where nothing dries the air out.
    Mean equator;
    Mean subtropics;
    Mean subpolar;
    for (const CellId cell : cells_at(6)) {
        if (climate.terrain().is_land(cell)) {
            continue;
        }
        const double sine = sine_of_latitude(cell);
        const double area = hog::planet::cell_area(cell);
        const auto moisture = static_cast<double>(climate.moisture(cell)) / fixed_one;
        if (sine < 0.1) {
            equator.add(moisture, area);
        } else if (sine > 0.45 && sine < 0.55) {
            subtropics.add(moisture, area);
        } else if (sine > 0.84 && sine < 0.89) {
            subpolar.add(moisture, area);
        }
    }
    CHECK(equator.value() - subtropics.value() > 0.3);
    CHECK(subpolar.value() - subtropics.value() > 0.3);
}

TEST_CASE("the interior is drier than the coast", "[planet][climate]") {
    const std::uint64_t seed = GENERATE(1U, 2U);
    const Climate climate{planet(seed)};
    Mean coast;
    Mean interior;
    for (const CellId cell : cells_at(6)) {
        const std::int64_t height = climate.terrain().height(cell);
        const double area = hog::planet::cell_area(cell);
        const auto moisture = static_cast<double>(climate.moisture(cell)) / fixed_one;
        if (height > 0 && height < fixed_one / 20) {
            coast.add(moisture, area);
        } else if (height > fixed_one * 3 / 10) {
            interior.add(moisture, area);
        }
    }
    CHECK(coast.value() - interior.value() > 0.1);
}

TEST_CASE("biomes cover the land in plausible shares", "[planet][climate]") {
    std::set<Biome> seen;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        INFO("seed " << seed);
        const Climate climate{planet(seed)};
        std::array<double, hog::planet::biome_count> areas{};
        double land = 0.0;
        double desert_in_subtropics = 0.0;
        for (const CellId cell : cells_at(6)) {
            const Biome biome = climate.biome(cell);
            REQUIRE((biome == Biome::Ocean) == !climate.terrain().is_land(cell));
            seen.insert(biome);
            if (biome == Biome::Ocean) {
                continue;
            }
            const double area = hog::planet::cell_area(cell);
            const double sine = sine_of_latitude(cell);
            areas.at(static_cast<std::size_t>(biome)) += area;
            land += area;
            if (biome == Biome::Desert && sine > 0.25 && sine < 0.71) {
                desert_in_subtropics += area;
            }
            if (biome == Biome::Ice) {
                // Ice is polar or high up.
                CHECK((sine > 0.8 || climate.terrain().elevation(cell) > 1'000 * fixed_one));
            }
        }
        for (int biome = 1; biome < hog::planet::biome_count; ++biome) {
            INFO(to_string(static_cast<Biome>(biome)));
            CHECK(areas.at(static_cast<std::size_t>(biome)) < land * 0.35);
        }
        const double desert = areas.at(static_cast<std::size_t>(Biome::Desert));
        CHECK(desert > land * 0.03);
        CHECK(desert < land * 0.2);
        // Deserts gather in the dry belts around 30 degrees.
        CHECK(desert_in_subtropics > desert * 0.7);
    }
    CHECK(seen.size() == hog::planet::biome_count);
}

TEST_CASE("climate changes smoothly between neighbours", "[planet][climate]") {
    const Climate climate{planet(2)};
    constexpr int level = 16;
    constexpr std::uint64_t side = std::uint64_t{1} << level;
    std::int64_t largest_temperature_step = 0;
    std::int64_t largest_moisture_step = 0;
    for (std::uint64_t sample = 0; sample < 5'000; ++sample) {
        const CellId cell = CellId::from_face_ij(
            static_cast<int>(hog::hash_values(sample, 0) % hog::planet::face_count),
            static_cast<std::uint32_t>(hog::hash_values(sample, 1) % side),
            static_cast<std::uint32_t>(hog::hash_values(sample, 2) % side), level);
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            largest_temperature_step =
                std::max(largest_temperature_step,
                         std::abs(climate.temperature(cell) - climate.temperature(next)));
            largest_moisture_step = std::max(
                largest_moisture_step, std::abs(climate.moisture(cell) - climate.moisture(next)));
        }
    }
    CHECK(largest_temperature_step < fixed_one / 10);
    CHECK(largest_moisture_step < fixed_one / 1'000);
}

TEST_CASE("climate depends on the seed", "[planet][climate]") {
    const CellId cell = CellId::from_face_ij(2, 100, 200, 10);
    const Climate first{planet(1)};
    CHECK(Climate{planet(1)}.temperature(cell) == first.temperature(cell));
    CHECK(Climate{planet(1)}.moisture(cell) == first.moisture(cell));
    CHECK(Climate{planet(2)}.temperature(cell) != first.temperature(cell));
    CHECK(Climate{planet(2)}.moisture(cell) != first.moisture(cell));
}
