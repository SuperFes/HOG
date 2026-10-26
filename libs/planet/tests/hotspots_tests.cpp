#include <hog/planet/continents.hpp>
#include <hog/planet/hotspots.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

using hog::planet::Continents;
using hog::planet::Hotspots;
using hog::planet::Plates;

namespace {

constexpr double radius = 115'000.0;
constexpr double spin = 50'000.0 / radius;
hog::DVec3 pole() {
    return {0.0, 0.0, 1.0};
}

// An ocean planet of two plates grown from opposite sides, turning about the pole.
Continents ocean() {
    return Continents{
        1, 0.9,
        std::make_shared<const Plates>(
            1, hog::planet::LevelGrid{Plates::level, radius}, radius,
            std::vector<Plates::Plate>{
                {.origin = {1.0, 0.0, 0.0}, .spin = {0.0, 0.0, spin}, .continental = -0.8},
                {.origin = {-1.0, 0.0, 0.0}, .spin = {0.0, 0.0, -spin}, .continental = -0.8}})};
}

double apart(hog::DVec3 from, hog::DVec3 to) {
    return glm::length(to - from) * radius;
}

// `metres` from `from` towards `towards`.
hog::DVec3 along(hog::DVec3 from, hog::DVec3 towards, double metres) {
    const hog::DVec3 across = glm::normalize(towards - (from * glm::dot(towards, from)));
    return glm::normalize(from + (across * (metres / radius)));
}

std::optional<double> surface(const Hotspots& hotspots, hog::DVec3 at) {
    const auto found = hotspots.surface(hog::planet::cell_at(at));
    if (!found) {
        return std::nullopt;
    }
    return hog::planet::fixed_to_double(*found);
}

} // namespace

TEST_CASE("plumes rise under deep open ocean, inside plates and apart", "[planet][hotspots]") {
    const Continents found = ocean();
    const Hotspots hotspots{1, radius, found};
    REQUIRE(hotspots.hotspots().size() == Hotspots::count);
    CHECK(hotspots.shields().size() == Hotspots::count * Hotspots::chain_length);
    const auto& plates = found.plates();
    for (std::size_t first = 0; first < hotspots.hotspots().size(); ++first) {
        const hog::DVec3 center = hotspots.hotspots()[first].center;
        CHECK(found.crust(hog::planet::cell_at(center)).height < -hog::planet::fixed_one / 5);
        const auto index = plates.grid().index(hog::planet::cell_at(center, Plates::level));
        CHECK(hog::planet::fixed_to_double(plates.boundaries()[index].distance) >= 30'000.0);
        for (std::size_t second = first + 1; second < hotspots.hotspots().size(); ++second) {
            CHECK(apart(center, hotspots.hotspots()[second].center) >= 150'000.0);
        }
    }
}

TEST_CASE("chains trail the way the plate carries them", "[planet][hotspots]") {
    const Continents found = ocean();
    const Hotspots hotspots{1, radius, found};
    const auto& plates = found.plates();
    for (std::size_t plume = 0; plume < hotspots.hotspots().size(); ++plume) {
        const auto& hotspot = hotspots.hotspots()[plume];
        const auto index = plates.grid().index(hog::planet::cell_at(hotspot.center, Plates::level));
        const hog::DVec3 velocity = plates.velocity(plates.plate()[index], hotspot.center);
        CHECK(glm::dot(hotspot.heading, glm::normalize(velocity)) > 0.999);
        const auto chain =
            hotspots.shields().subspan(plume * Hotspots::chain_length, Hotspots::chain_length);
        CHECK(apart(chain.front().center, hotspot.center) < 2'000.0);
        for (std::size_t age = 1; age < chain.size(); ++age) {
            CHECK(apart(chain[age - 1].center, chain[age].center) > 9'000.0);
            CHECK(apart(chain[age - 1].center, chain[age].center) < 15'000.0);
            CHECK(apart(chain[age].center, hotspot.center) >
                  apart(chain[age - 1].center, hotspot.center));
            // Downstream of the plume.
            CHECK(glm::dot(chain[age].center - hotspot.center, hotspot.heading) > 0.0);
        }
    }
}

TEST_CASE("shields sink as they age, from islands to seamounts", "[planet][hotspots]") {
    const Hotspots hotspots{1, radius, ocean()};
    for (std::size_t plume = 0; plume < hotspots.hotspots().size(); ++plume) {
        const auto chain =
            hotspots.shields().subspan(plume * Hotspots::chain_length, Hotspots::chain_length);
        CHECK(chain.front().summit == Hotspots::young_summit);
        CHECK(chain.back().summit < -1'000.0);
        for (std::size_t age = 1; age < chain.size(); ++age) {
            CHECK(chain[age].summit < chain[age - 1].summit - 200.0);
        }
    }
}

TEST_CASE("a shield's flanks fall ever steeper from its caldera", "[planet][hotspots]") {
    const Hotspots hotspots{1, radius, ocean()};
    REQUIRE_FALSE(hotspots.hotspots().empty());
    const auto& hotspot = hotspots.hotspots().front();
    const auto& young = hotspots.shields().front();
    // Away from the rest of the chain.
    const hog::DVec3 out = young.center - hotspot.heading;
    // Where none reaches, lower than any flank, so the checks below fail.
    const auto at = [&](double metres) {
        return surface(hotspots, along(young.center, out, metres))
            .value_or(std::numeric_limits<double>::lowest());
    };
    CHECK(at(0.0) == Catch::Approx(young.summit - 150.0).margin(0.01));
    CHECK(at(0.0) < at(800.0));
    CHECK(at(800.0) > young.summit - 100.0);
    // Whichever way the rift zones push the flank, and however rough.
    for (int step = 3; step < 11; ++step) {
        CHECK(at((step + 1) * 1'000.0) < at(step * 1'000.0));
    }
    CHECK(at(1'000.0) - at(2'000.0) < at(10'000.0) - at(11'000.0));
    // An island, its coast some kilometres out, its foot deep below.
    CHECK(at(2'000.0) > 0.0);
    CHECK(at(14'000.0) < -1'000.0);
    // Nor anywhere none reaches.
    for (const hog::DVec3 away :
         {hog::DVec3{1.0, 0.0, 0.0}, hog::DVec3{-1.0, 0.0, 0.0}, hog::DVec3{0.0, 1.0, 0.0},
          hog::DVec3{0.0, -1.0, 0.0}, pole(), -pole()}) {
        const bool reached = std::ranges::any_of(hotspots.shields(), [&](const auto& shield) {
            return apart(shield.center, away) < 23'000.0;
        });
        if (!reached) {
            CHECK_FALSE(surface(hotspots, away));
        }
    }
}

TEST_CASE("the floor swells around a plume", "[planet][hotspots]") {
    const Hotspots hotspots{1, radius, ocean()};
    REQUIRE_FALSE(hotspots.hotspots().empty());
    const auto swell = [&](hog::DVec3 at) {
        return hog::planet::fixed_to_double(hotspots.swell(hog::planet::cell_at(at)));
    };
    const hog::DVec3 center = hotspots.hotspots().front().center;
    const hog::DVec3 out = hotspots.hotspots().front().heading;
    CHECK(swell(center) == Catch::Approx(Hotspots::swell_height).epsilon(0.01));
    CHECK(swell(along(center, center + out, 20'000.0)) < swell(center));
    CHECK(swell(along(center, center + out, 20'000.0)) > 0.0);
    CHECK(swell(along(center, center + out, 45'000.0)) == 0.0);
}

TEST_CASE("hotspots depend on the seed alone", "[planet][hotspots]") {
    const Continents found = ocean();
    const Hotspots first{1, radius, found};
    const Hotspots again{1, radius, found};
    const Hotspots other{2, radius, found};
    REQUIRE(first.shields().size() == again.shields().size());
    for (std::size_t index = 0; index < first.shields().size(); ++index) {
        CHECK(first.shields()[index].center == again.shields()[index].center);
        CHECK(first.shields()[index].summit == again.shields()[index].summit);
    }
    REQUIRE_FALSE(other.hotspots().empty());
    CHECK(other.hotspots().front().center != first.hotspots().front().center);
}
