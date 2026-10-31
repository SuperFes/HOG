#include <hog/globe/globe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>

using hog::globe::Globe;
using hog::globe::HexCell;
using hog::globe::Hexel;
using hog::globe::Material;

TEST_CASE("every material is denser than air, and iron the densest", "[globe][material]") {
    for (int index = 1; index < hog::globe::material_count; ++index) {
        const auto material = static_cast<Material>(index);
        CHECK(hog::globe::properties(material).density >
              hog::globe::properties(Material::Air).density);
        CHECK(hog::globe::properties(material).density <=
              hog::globe::properties(Material::Iron).density);
        CHECK(hog::globe::properties(material).melts > 0.0);
    }
}

TEST_CASE("a globe keeps each column's hexels apart, the lowest shell first", "[globe]") {
    Globe globe{2, {.inner = 1'000.0, .thickness = 10.0, .count = 4}};
    CHECK(globe.shells().outer() == 1'040.0);
    const HexCell one = hog::globe::cell_at_index(2, 5);
    const HexCell two = hog::globe::cell_at_index(2, 6);
    globe.at(one, 0) = {.material = Material::Granite, .temperature = 300};
    globe.at(one, 3) = {.material = Material::Water, .temperature = 280};
    CHECK(globe.column(one).size() == 4);
    CHECK(globe.column(one)[0] == Hexel{.material = Material::Granite, .temperature = 300});
    CHECK(globe.column(one)[3].material == Material::Water);
    CHECK(globe.column(two)[0] == Hexel{});
    CHECK(globe.shells().floor(3) == 1'030.0);
    CHECK(globe.shells().ceiling(3) == 1'040.0);
}

TEST_CASE("a globe weighs its materials by the space they fill", "[globe]") {
    constexpr double radius = 2'000.0;
    Globe globe{3, {.inner = 0.0, .thickness = radius / 8.0, .count = 8}};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(3); ++index) {
        for (Hexel& hexel : globe.column(hog::globe::cell_at_index(3, index))) {
            hexel.material = Material::Mantle;
        }
    }
    const auto masses = globe.masses();
    const double ball = 4.0 / 3.0 * std::numbers::pi * radius * radius * radius *
                        hog::globe::properties(Material::Mantle).density;
    CHECK(std::abs(masses[static_cast<std::size_t>(Material::Mantle)] - ball) < 1e-9 * ball);
    CHECK(masses[static_cast<std::size_t>(Material::Air)] == 0.0);
}
