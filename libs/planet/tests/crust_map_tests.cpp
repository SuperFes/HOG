#include <hog/planet/crust_map.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>

using hog::planet::CrustMap;
using hog::planet::LevelGrid;
using hog::planet::Plates;

namespace {

constexpr double radius = 115'000.0;
constexpr double ocean_fraction = 0.7;

CrustMap crust_of(std::uint64_t seed, CrustMap::Settings settings = {}) {
    return CrustMap{
        seed, std::make_shared<const Plates>(seed, LevelGrid{Plates::level, radius}, radius, 10),
        settings, ocean_fraction};
}

double planet_area() {
    return 4.0 * std::numbers::pi * radius * radius;
}

} // namespace

TEST_CASE("Continents cover about the share of the planet they're given", "[crust_map]") {
    const auto seed = GENERATE(1U, 7U, 42U);
    const CrustMap::Settings settings{.continental = 0.35};
    const CrustMap crust = crust_of(seed, settings);
    double continental = 0.0;
    for (std::uint32_t index = 0; index < crust.grid().size(); ++index) {
        if (crust.columns()[index].kind == CrustMap::Kind::Continental) {
            continental += static_cast<double>(crust.grid().area(index));
        }
    }
    // Within a few terranes of it.
    const double terrane = settings.terrane_size * settings.terrane_size;
    CHECK(std::abs(continental - (settings.continental * planet_area())) < 4.0 * terrane);
}

TEST_CASE("The same seed makes the same crust", "[crust_map]") {
    const CrustMap first = crust_of(3);
    const CrustMap second = crust_of(3);
    REQUIRE(first.columns().size() == second.columns().size());
    for (std::size_t index = 0; index < first.columns().size(); ++index) {
        CHECK(first.columns()[index].thickness == second.columns()[index].thickness);
        CHECK(first.heights()[index] == second.heights()[index]);
    }
}

TEST_CASE("Thickness is the settled crust plus what was piled up and pulled apart", "[crust_map]") {
    const CrustMap crust = crust_of(11);
    const auto& settings = crust.settings();
    for (const CrustMap::Column& column : crust.columns()) {
        if (column.kind == CrustMap::Kind::Oceanic) {
            CHECK_THAT(column.thickness - column.active,
                       Catch::Matchers::WithinAbs(settings.oceanic_thickness, 1e-9));
            CHECK(column.worn == 0.0);
            continue;
        }
        const double settled = column.thickness - column.active - column.worn;
        CHECK(settled >= settings.margin_thickness - 1e-9);
        CHECK(settled <= settings.continental_thickness + settings.terrane_spread + 1e-9);
        CHECK(column.thickness >= 2.0 - 1e-9);
    }
}

TEST_CASE("Crust rises and falls only near boundaries doing so", "[crust_map]") {
    const CrustMap crust = crust_of(5);
    const auto& settings = crust.settings();
    const double widest =
        std::max({settings.collision_reach, settings.cordillera_inland + settings.cordillera_reach,
                  settings.arc_inland + settings.arc_reach, settings.rift_reach});
    for (std::uint32_t index = 0; index < crust.grid().size(); ++index) {
        const CrustMap::Column& column = crust.columns()[index];
        const auto& boundary = crust.plates().boundaries()[index];
        if (column.active > 0.0) {
            CHECK(boundary.closing > 0.0);
            CHECK(hog::planet::fixed_to_double(boundary.distance) <= widest + 1.0);
        } else if (column.active < 0.0) {
            CHECK(boundary.closing < 0.0);
            CHECK(column.kind == CrustMap::Kind::Continental);
        }
    }
}

TEST_CASE("Ocean floor is young at its ridges and older away from them", "[crust_map]") {
    const CrustMap crust = crust_of(9);
    const auto boundaries = crust.plates().boundaries();
    int ridges = 0;
    for (std::uint32_t index = 0; index < crust.grid().size(); ++index) {
        const CrustMap::Column& column = crust.columns()[index];
        if (column.kind == CrustMap::Kind::Oceanic && boundaries[index].distance == 0 &&
            boundaries[index].closing < 0.0) {
            CHECK(column.age == 0.0);
            ++ridges;
        }
    }
    CHECK(ridges > 0);
}

TEST_CASE("Crust floats higher the thicker it is and ocean floor sinks as it ages", "[crust_map]") {
    const CrustMap crust = crust_of(1);
    using Kind = CrustMap::Kind;
    CHECK(crust.floated(Kind::Continental, 50.0, 0.0) >
          crust.floated(Kind::Continental, 35.0, 0.0));
    CHECK(crust.floated(Kind::Oceanic, 7.0, 80.0) < crust.floated(Kind::Oceanic, 7.0, 10.0));
    CHECK(crust.floated(Kind::Oceanic, 7.0, 0.0) == 0.0);
    // Settled continent stands as high above a ridge as its freeboard has it.
    CHECK(crust.floated(Kind::Continental, 35.0, 0.0) == hog::planet::continent_freeboard);
}

TEST_CASE("The sea covers at least the share of the planet it's topped up to", "[crust_map]") {
    const CrustMap crust = crust_of(13);
    double under = 0.0;
    for (std::uint32_t index = 0; index < crust.grid().size(); ++index) {
        if (crust.heights()[index] < crust.sea_level()) {
            under += static_cast<double>(crust.grid().area(index));
        }
    }
    CHECK(under / planet_area() >= ocean_fraction - 0.01);
}

TEST_CASE("Standing crust leaves out only what boundaries pile up now", "[crust_map]") {
    const CrustMap crust = crust_of(17);
    for (std::uint32_t index = 0; index < crust.grid().size(); ++index) {
        const CrustMap::Column& column = crust.columns()[index];
        if (column.active > 0.0) {
            CHECK(crust.standing()[index] <= crust.heights()[index]);
        } else if (column.active == 0.0) {
            CHECK(crust.standing()[index] == crust.heights()[index]);
        }
    }
}
