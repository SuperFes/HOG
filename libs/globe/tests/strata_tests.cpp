#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;

namespace {

Generator generator_for(std::string_view name) {
    return Generator{hog::globe::preset(name).value_or(hog::globe::Planet{})};
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

TEST_CASE("the ground is bare rock over beds of rock, with no loose cover of its own",
          "[globe][strata]") {
    for (const std::string_view name : {"earth", "moon", "mars"}) {
        CAPTURE(name);
        const Found found = dig(generator_for(name), 300);
        for (const Material loose : {Material::Soil, Material::Sediment, Material::Snow,
                                     Material::Regolith, Material::Sand}) {
            CHECK_FALSE(found.top.contains(loose));
            CHECK_FALSE(found.deep.contains(loose));
        }
    }
    const Found earth = dig(generator_for("earth"), 400);
    for (const Material material :
         {Material::Limestone, Material::Shale, Material::Sandstone, Material::Basalt}) {
        CHECK(earth.deep.contains(material));
    }
    // Mars once had water: beds of sandstone and shale, but no seas to lay limestone.
    const Found mars = dig(generator_for("mars"), 300);
    CHECK(mars.deep.contains(Material::Sandstone));
    CHECK_FALSE(mars.deep.contains(Material::Limestone));
    CHECK_FALSE(dig(generator_for("moon"), 300).deep.contains(Material::Sandstone));
}

TEST_CASE("the ground under the sea is soaked to its pores, and dry above it", "[globe][strata]") {
    const Generator earth = generator_for("earth");
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
