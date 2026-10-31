#include <hog/globe/accretion.hpp>
#include <hog/globe/birth.hpp>
#include <hog/globe/differentiation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::Accretion;
using hog::globe::Globe;
using hog::globe::Hexel;
using hog::globe::Material;

namespace {

// A planet as big as the game's, in coarse hexels, hot enough outside to melt.
const Accretion setup{.seed = 3, .level = 4};
const hog::globe::Gravity pull{.surface = setup.gravity, .radius = setup.radius};

struct Sorted {
    Globe young;
    Globe sorted;
    std::uint64_t sunk = 0;
};

const Sorted& sorted() {
    static const Sorted made = [] {
        Globe young = hog::globe::accrete(setup);
        Globe sorted = young;
        const std::uint64_t sunk = hog::globe::differentiate(sorted, pull);
        return Sorted{.young = std::move(young), .sorted = std::move(sorted), .sunk = sunk};
    }();
    return made;
}

// The share of each shell's hexels that are iron.
std::vector<double> iron_by_shell(const Globe& globe) {
    std::vector<double> iron(static_cast<std::size_t>(globe.shells().count), 0.0);
    std::vector<double> solid(iron.size(), 0.0);
    for (std::uint64_t index = 0; index < hog::globe::cell_count(globe.level()); ++index) {
        const auto column = globe.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            if (column[shell].material != Material::Air) {
                solid[shell] += 1.0;
                iron[shell] += column[shell].material == Material::Iron ? 1.0 : 0.0;
            }
        }
    }
    for (std::size_t shell = 0; shell < iron.size(); ++shell) {
        iron[shell] = solid[shell] > 0.0 ? iron[shell] / solid[shell] : 0.0;
    }
    return iron;
}

double mean_temperature(const Globe& globe) {
    double sum = 0.0;
    double count = 0.0;
    for (std::uint64_t index = 0; index < hog::globe::cell_count(globe.level()); ++index) {
        for (const Hexel& hexel : globe.column_at(index)) {
            if (hexel.material != Material::Air) {
                sum += hexel.temperature;
                count += 1.0;
            }
        }
    }
    return sum / count;
}

} // namespace

TEST_CASE("the iron sinks to a core and the rock rises over it", "[globe][differentiation]") {
    CHECK(sorted().sunk > 0);
    // The iron fills the fewest of the lowest shells it can.
    const Globe& globe = sorted().sorted;
    const auto columns = static_cast<double>(hog::globe::cell_count(globe.level()));
    const auto iron = iron_by_shell(globe);
    double hexels = 0.0;
    for (const double share : iron) {
        hexels += share * columns;
    }
    const auto needed = static_cast<std::size_t>(std::ceil(hexels / columns));
    double inside = 0.0;
    for (std::size_t shell = 0; shell < needed; ++shell) {
        inside += iron[shell] * columns;
    }
    CHECK(inside > 0.97 * hexels);
    CHECK(iron[0] > 0.7);
    CHECK(iron[iron.size() - 3] == 0.0);
}

TEST_CASE("sorting keeps every hexel of every material", "[globe][differentiation]") {
    CHECK(sorted().sorted.masses() == sorted().young.masses());
}

TEST_CASE("the iron's fall heats the planet", "[globe][differentiation]") {
    CHECK(mean_temperature(sorted().sorted) > mean_temperature(sorted().young) + 20.0);
}

TEST_CASE("molten rock lets its water and gas go, and none is lost", "[globe][differentiation]") {
    const auto& young = sorted().young;
    const auto& grown = sorted().sorted;
    CHECK(grown.freed().water > 0.0);
    CHECK(grown.freed().gas > 0.0);
    const double water = grown.held().water + grown.freed().water;
    const double gas = grown.held().gas + grown.freed().gas;
    CHECK(std::abs(water - young.held().water) < 1e-9 * water);
    CHECK(std::abs(gas - young.held().gas) < 1e-9 * gas);
}

TEST_CASE("a planet is born gathered and sorted", "[globe][birth]") {
    const Globe born = hog::globe::birth(setup);
    CHECK(born.masses() == sorted().sorted.masses());
    CHECK(iron_by_shell(born)[0] > 0.7);
    CHECK(born.freed() == sorted().sorted.freed());
}
