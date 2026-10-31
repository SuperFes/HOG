#include <hog/globe/settling.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::Globe;
using hog::globe::Gravity;
using hog::globe::HexCell;
using hog::globe::Hexel;
using hog::globe::Material;

namespace {

constexpr int level = 4;
constexpr double radius = 100'000.0;

// Ground to the radius in shells about `thickness` thick there, and room above.
Globe flat(double thickness, std::uint16_t temperature) {
    const auto shells = hog::globe::shells_about(std::max(0.0, radius - (200.0 * thickness)),
                                                 radius + (40.0 * thickness), radius, thickness);
    Globe globe{level, shells};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level); ++index) {
        for (int shell = 0; shell < shells.count && shells.ceiling(shell) <= radius; ++shell) {
            globe.at(hog::globe::cell_at_index(level, index),
                     shell) = {.material = Material::Mantle, .temperature = temperature};
        }
    }
    return globe;
}

void raise(Globe& globe, HexCell cell, int shells, Hexel hexel) {
    const auto column = globe.column(cell);
    const int top = hog::globe::top_of(column);
    for (int shell = top; shell < top + shells; ++shell) {
        column[static_cast<std::size_t>(shell)] = hexel;
    }
}

std::uint64_t solid(const Globe& globe) {
    std::uint64_t count = 0;
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level); ++index) {
        for (const Hexel& hexel : globe.column(hog::globe::cell_at_index(level, index))) {
            count += hexel.material == Material::Air ? 0 : 1;
        }
    }
    return count;
}

const Gravity pull{.surface = 9.81, .radius = radius};

} // namespace

TEST_CASE("rock bears a face as tall as its strength holds against its weight",
          "[globe][settling]") {
    const Hexel granite{.material = Material::Granite, .temperature = 280};
    // 130 MPa under 2700 kg/m³ at 9.81 m/s²: about 4.9 km.
    CHECK(hog::globe::bearable(granite, 9.81) > 4'800.0);
    CHECK(hog::globe::bearable(granite, 9.81) < 5'000.0);
    // Near melting it bears little, molten nothing, and sand nothing at all.
    CHECK(hog::globe::bearable({.material = Material::Granite, .temperature = 1'400}, 9.81) <
          1'000.0);
    CHECK(hog::globe::bearable({.material = Material::Granite, .temperature = 1'600}, 9.81) == 0.0);
    CHECK(hog::globe::bearable({.material = Material::Sand, .temperature = 280}, 9.81) == 0.0);
}

TEST_CASE("a pillar of hot rock slumps out across the ground, keeping every hexel",
          "[globe][settling]") {
    Globe globe = flat(2'000.0, 1'300);
    const HexCell middle = hog::globe::cell_at_index(level, 400);
    raise(globe, middle, 12, {.material = Material::Mantle, .temperature = 1'300});
    const std::uint64_t before = solid(globe);
    const auto masses = globe.masses();
    const std::vector<HexCell> start{middle};
    CHECK(hog::globe::settle(globe, start, pull) > 0);
    CHECK(solid(globe) == before);
    CHECK(globe.masses() == masses);
    // No column now stands two or more shells over a neighbour.
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level); ++index) {
        const HexCell cell = hog::globe::cell_at_index(level, index);
        const int top = hog::globe::top_of(globe.column(cell));
        const auto around = hog::globe::neighbors(cell);
        for (const HexCell next : around.all()) {
            CHECK(top - hog::globe::top_of(globe.column(next)) < 2);
        }
    }
}

TEST_CASE("a cold crag stands where loose sand of its height pours away", "[globe][settling]") {
    Globe globe = flat(100.0, 280);
    const HexCell crag = hog::globe::cell_at_index(level, 400);
    raise(globe, crag, 10, {.material = Material::Granite, .temperature = 280});
    const std::vector<HexCell> start{crag};
    CHECK(hog::globe::settle(globe, start, pull) == 0);

    Globe dune = flat(100.0, 280);
    raise(dune, crag, 10, {.material = Material::Sand, .temperature = 280});
    CHECK(hog::globe::settle(dune, start, pull) > 0);
    CHECK(hog::globe::top_of(dune.column(crag)) < hog::globe::top_of(globe.column(crag)));
}
