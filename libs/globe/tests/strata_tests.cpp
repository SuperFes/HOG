#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;

namespace {

Generator wet() {
    return Generator{hog::globe::Planet{}};
}

// Too cold and thin-aired for seas.
Generator frozen() {
    hog::globe::Planet planet;
    planet.pressure = 636.0;
    planet.equator_temperature = 224.0;
    planet.pole_temperature = 150.0;
    return Generator{planet};
}

DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
}

// What each column holds from the ground down, by depth, over many columns.
struct Found {
    std::set<Material> top;  // just under the ground
    std::set<Material> deep; // anywhere in the first kilometre
};

Found dig(const Generator& world, std::uint64_t count) {
    Found found;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 down = direction(k);
        const double ground = hog::globe::ground_below(world, down, 1.0).value_or(0.0);
        const Generator::Column column = world.column(down, 1.0);
        found.top.insert(column.sample(ground - 0.25).material);
        // Ever deeper, a third more each step, to a kilometre.
        for (int step = 0; step < 32; ++step) {
            found.deep.insert(column.sample(ground - (0.25 * std::pow(1.3, step))).material);
        }
    }
    return found;
}

} // namespace

TEST_CASE("beds of rock lie under the ground, limestone only where seas laid it",
          "[globe][strata]") {
    const Found earth = dig(wet(), 400);
    for (const Material material :
         {Material::Limestone, Material::Shale, Material::Sandstone, Material::Basalt}) {
        CHECK(earth.deep.contains(material));
    }
    // With no seas, beds of sandstone and shale but no limestone.
    const Found dry = dig(frozen(), 300);
    CHECK(dry.deep.contains(Material::Sandstone));
    CHECK_FALSE(dry.deep.contains(Material::Limestone));
}

TEST_CASE("the ground under the sea is soaked to its pores, and dry above it", "[globe][strata]") {
    const Generator earth = wet();
    const std::optional<double> level = earth.sea_level();
    REQUIRE(level);
    const double sea = level.value_or(0.0);
    int soaked = 0;
    int dry = 0;
    for (std::uint64_t k = 0; k < 300; ++k) {
        const DVec3 down = direction(k);
        const double ground = hog::globe::ground_below(earth, down, 1.0).value_or(0.0);
        const hog::globe::Sample under = earth.sample(down * (ground - 1.0), 1.0);
        REQUIRE(under.distance < 0.0);
        if (ground < sea) {
            CHECK(under.liquid == Material::Water);
            CHECK(under.water == hog::globe::properties(under.material).porosity);
            CHECK(under.water > 0.0);
            ++soaked;
        } else if (ground > sea + 50.0) {
            CHECK(under.water == 0.0);
            CHECK(under.liquid == Material::Air);
            ++dry;
        }
    }
    CHECK(soaked > 50);
    CHECK(dry > 50);
}
