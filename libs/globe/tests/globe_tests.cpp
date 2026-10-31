#include <hog/globe/globe.hpp>
#include <hog/globe/hex_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    Globe globe{2, {.inner = 1'000.0, .outer = 1'040.0, .count = 4}};
    const HexCell one = hog::globe::cell_at_index(2, 5);
    const HexCell two = hog::globe::cell_at_index(2, 6);
    globe.at(one, 0) = {.material = Material::Granite, .temperature = 300};
    globe.at(one, 3) = {.material = Material::Water, .temperature = 280};
    CHECK(globe.column(one).size() == 4);
    CHECK(globe.column(one)[0] == Hexel{.material = Material::Granite, .temperature = 300});
    CHECK(globe.column(one)[3].material == Material::Water);
    CHECK(globe.column(two)[0] == Hexel{});
    CHECK(globe.shells().floor(0) == 1'000.0);
    CHECK(globe.shells().ceiling(3) == 1'040.0);
    // Equal volumes thin outward.
    CHECK(globe.shells().thickness(3) < globe.shells().thickness(0));
    const auto cube = [](double r) { return r * r * r; };
    CHECK(std::abs(cube(globe.shells().ceiling(1)) - cube(globe.shells().floor(1)) -
                   (cube(globe.shells().ceiling(2)) - cube(globe.shells().floor(2)))) < 1e-3);
}

TEST_CASE("a globe weighs its materials by the space they fill", "[globe]") {
    constexpr double radius = 2'000.0;
    Globe globe{3, {.inner = 0.0, .outer = radius, .count = 8}};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(3); ++index) {
        for (Hexel& hexel : globe.column(hog::globe::cell_at_index(3, index))) {
            hexel.material = Material::Mantle;
        }
    }
    const auto masses = globe.masses();
    // Pentagons' hexels count as whole ones: the twelve add two hexels' worth to each shell.
    const double ball = 4.0 / 3.0 * std::numbers::pi * radius * radius * radius *
                        hog::globe::properties(Material::Mantle).density;
    const double over = 2.0 / static_cast<double>(hog::globe::cell_count(3) - 2);
    CHECK(std::abs(masses[static_cast<std::size_t>(Material::Mantle)] - (ball * (1.0 + over))) <
          1e-9 * ball);
    CHECK(masses[static_cast<std::size_t>(Material::Air)] == 0.0);
}

TEST_CASE("finer shells split each shell in two, their floors landing on its", "[globe]") {
    const hog::globe::Shells coarse{.inner = 0.0, .outer = 143'750.0, .count = 19};
    const hog::globe::Shells fine = coarse.finer(12);
    CHECK(fine.count == 19 << 12);
    for (int shell = 0; shell <= coarse.count; ++shell) {
        CHECK(fine.floor(shell << 12) == coarse.floor(shell));
    }
}

TEST_CASE("a globe a level finer has eight hexels for each of its own", "[globe]") {
    constexpr int level = 3;
    Globe coarse{level, {.inner = 0.0, .outer = 2'000.0, .count = 6}};
    std::array<std::int64_t, hog::globe::material_count> want{};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level); ++index) {
        const bool pentagon = hog::globe::is_pentagon(hog::globe::cell_at_index(level, index));
        const auto column = coarse.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            const std::size_t pick = ((index * 7) + (shell * 3)) % 4;
            const std::array materials{Material::Air, Material::Water, Material::Granite,
                                       Material::Iron};
            column[shell] = {.material = materials.at(pick),
                             .held = static_cast<std::uint8_t>(index % 200),
                             .temperature = static_cast<std::uint16_t>(300 + shell)};
            want.at(static_cast<std::size_t>(column[shell].material)) += pentagon ? 7 : 8;
        }
    }
    const Globe fine = hog::globe::refined(coarse);
    CHECK(fine.level() == level + 1);
    CHECK(fine.shells().count == 12);
    CHECK(std::abs(fine.hexel_volume() - (coarse.hexel_volume() / 8.0)) <
          1e-12 * coarse.hexel_volume());
    std::array<std::int64_t, hog::globe::material_count> got{};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level + 1); ++index) {
        const HexCell cell = hog::globe::cell_at_index(level + 1, index);
        const auto over = hog::globe::parents(cell);
        const auto column = fine.column(cell);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            ++got.at(static_cast<std::size_t>(column[shell].material));
            // Every fine hexel is one of its parents' in the shell it was split from.
            const int from = static_cast<int>(shell / 2);
            bool found = false;
            for (const HexCell parent : over.all()) {
                found = found || coarse.at(parent, from) == column[shell];
            }
            CHECK(found);
            if (shell % 2 == 1) {
                CHECK(hog::globe::properties(column[shell - 1].material).density >=
                      hog::globe::properties(column[shell].material).density);
            }
        }
    }
    CHECK(got == want);
}

TEST_CASE("a window holds some columns through a run of shells", "[globe]") {
    const hog::globe::Shells whole{.inner = 0.0, .outer = 2'000.0, .count = 20};
    const hog::globe::Shells part = whole.part(12, 18);
    CHECK(part.count == 6);
    for (int shell = 0; shell <= part.count; ++shell) {
        CHECK(std::abs(part.floor(shell) - whole.floor(12 + shell)) < 1e-9 * whole.outer);
    }
    const HexCell middle = hog::globe::cell_at_index(5, 4'321);
    const auto cells = hog::globe::disc(middle, 2);
    CHECK(cells.size() == 19);
    CHECK(cells.front() == middle);
    Globe near{5, part, cells, 12};
    CHECK(!near.is_whole());
    CHECK(near.size() == 19);
    CHECK(near.holds(middle));
    CHECK(!near.holds(hog::globe::cell_at_index(5, 1)));
    CHECK(near.index_of(middle) == 0);
    near.at(cells[7], 5) = {.material = Material::Basalt, .temperature = 900};
    CHECK(near.column_at(7)[5].material == Material::Basalt);
    CHECK(near.column(middle).size() == 6);

    // Its grid knows only the neighbours it holds.
    const hog::globe::HexGrid grid{near};
    CHECK(grid.size() == 19);
    CHECK(grid.neighbors(0).size() == 6);
    std::size_t sides = 0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        sides += grid.neighbors(index).size();
    }
    // Inside, every pair counts twice: the 6 spokes, 6 round the first ring, 18 to the second
    // and 12 round it.
    CHECK(sides == 2 * (6 + 6 + 18 + 12));
}
