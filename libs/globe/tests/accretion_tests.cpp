#include <hog/globe/accretion.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

using hog::globe::Accretion;
using hog::globe::Globe;
using hog::globe::HexCell;
using hog::globe::Hexel;
using hog::globe::Material;

namespace {

const Accretion setup{.seed = 3, .radius = 50'000.0, .level = 4};

const Globe& grown() {
    static const Globe globe = hog::globe::accrete(setup);
    return globe;
}

template <typename Each>
void for_each_column(const Globe& globe, Each each) {
    for (std::uint64_t index = 0; index < hog::globe::cell_count(globe.level()); ++index) {
        each(globe.column(hog::globe::hex_cell_at_index(globe.level(), index)));
    }
}

} // namespace

TEST_CASE("a planet grows the same from the same seed", "[globe][accretion]") {
    const Globe again = hog::globe::accrete(setup);
    bool same = true;
    for (std::uint64_t index = 0; index < hog::globe::cell_count(setup.level); ++index) {
        const HexCell cell = hog::globe::hex_cell_at_index(setup.level, index);
        same = same && std::ranges::equal(again.column(cell), grown().column(cell));
    }
    CHECK(same);
    Accretion other = setup;
    other.seed = 4;
    const Globe different = hog::globe::accrete(other);
    CHECK(different.masses() != grown().masses());
}

TEST_CASE("a planet grows solid from its centre to about its radius", "[globe][accretion]") {
    const auto& shells = grown().shells();
    double heights = 0.0;
    std::uint64_t columns = 0;
    for_each_column(grown(), [&](std::span<const Hexel> column) {
        std::size_t top = 0;
        while (top < column.size() && column[top].material != Material::Air) {
            ++top;
        }
        // Nothing hangs above a gap.
        for (std::size_t shell = top; shell < column.size(); ++shell) {
            CHECK(column[shell].material == Material::Air);
        }
        heights += shells.floor(static_cast<int>(top));
        ++columns;
    });
    const double mean = heights / static_cast<double>(columns);
    CHECK(std::abs(mean - setup.radius) < 1.5 * shells.thickness(shells.count / 2));
}

TEST_CASE("what falls is a third metal by mass and the rest rock holding its water and gas",
          "[globe][accretion]") {
    const auto masses = grown().masses();
    const double iron = masses[static_cast<std::size_t>(Material::Iron)];
    const double rock = masses[static_cast<std::size_t>(Material::Mantle)];
    CHECK(std::abs((iron / (iron + rock)) - setup.iron) < 0.02);
    for_each_column(grown(), [](std::span<const Hexel> column) {
        for (const Hexel& hexel : column) {
            if (hexel.material == Material::Mantle) {
                CHECK(hexel.held == 255);
            } else {
                CHECK(hexel.held == 0);
            }
        }
    });
}

TEST_CASE("the last to fall fell hardest, so a planet grows hotter outside than in",
          "[globe][accretion]") {
    const int shells = grown().shells().count;
    std::vector<double> sum(static_cast<std::size_t>(shells), 0.0);
    std::vector<int> count(static_cast<std::size_t>(shells), 0);
    for_each_column(grown(), [&](std::span<const Hexel> column) {
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            if (column[shell].material != Material::Air) {
                sum[shell] += column[shell].temperature;
                ++count[shell];
            }
        }
    });
    // The middle shell, and the last whose floor is still under the radius.
    const auto& layers = grown().shells();
    std::size_t near_top = 0;
    while (layers.floor(static_cast<int>(near_top) + 1) < setup.radius) {
        ++near_top;
    }
    const double core = sum[0] / count[0];
    const double outer = sum[near_top] / count[near_top];
    CHECK(core > setup.disc_temperature + setup.decay_heat);
    CHECK(outer > core + 100.0);
}

TEST_CASE("the rock holds water for a sea and gas for an air like Earth's", "[globe][accretion]") {
    const double surface = 4.0 * std::numbers::pi * setup.radius * setup.radius;
    const auto held = grown().held();
    CHECK(std::abs(held.water - (surface * setup.sea_depth * 1'000.0)) < 1e-9 * held.water);
    CHECK(std::abs(held.gas - (surface * setup.air_weight)) < 1e-9 * held.gas);
    CHECK(grown().freed() == hog::globe::Volatiles{});
}
