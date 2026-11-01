#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
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

TEST_CASE("earth's ground is soil, sand, mud and snow over beds of rock", "[globe][strata]") {
    const Found earth = dig(generator_for("earth"), 400);
    for (const Material material :
         {Material::Soil, Material::Sediment, Material::Snow, Material::Granite}) {
        CHECK(earth.top.contains(material));
    }
    for (const Material material :
         {Material::Limestone, Material::Shale, Material::Sandstone, Material::Basalt}) {
        CHECK(earth.deep.contains(material));
    }
    CHECK_FALSE(earth.deep.contains(Material::Regolith));
}

TEST_CASE("airless and dry worlds lie under regolith, never soil", "[globe][strata]") {
    const Found moon = dig(generator_for("moon"), 300);
    CHECK(moon.top.contains(Material::Regolith));
    CHECK_FALSE(moon.top.contains(Material::Soil));
    CHECK_FALSE(moon.deep.contains(Material::Sandstone));
    CHECK(moon.deep.contains(Material::Anorthosite));
    // Mars once had water: beds of sandstone and shale, but no seas to lay limestone.
    const Found mars = dig(generator_for("mars"), 300);
    CHECK(mars.top.contains(Material::Regolith));
    CHECK_FALSE(mars.top.contains(Material::Snow));
    CHECK(mars.deep.contains(Material::Sandstone));
    CHECK(mars.deep.contains(Material::Shale));
    CHECK_FALSE(mars.deep.contains(Material::Limestone));
}

TEST_CASE("regolith thickens with the surface's age", "[globe][strata]") {
    hog::globe::Planet young = hog::globe::preset("moon").value_or(hog::globe::Planet{});
    young.surface_age = 0.5;
    const Generator moon = generator_for("moon");
    const Generator fresh{young};
    // On gentle ground, about 2.5 m a billion years.
    int deeper = 0;
    constexpr int count = 100;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 down = direction(k);
        const double old_ground = hog::globe::ground_below(moon, down, 1.0).value_or(0.0);
        const double new_ground = hog::globe::ground_below(fresh, down, 1.0).value_or(0.0);
        const bool old_loose =
            moon.sample(down * (old_ground - 3.0), 1.0).material == Material::Regolith;
        const bool new_loose =
            fresh.sample(down * (new_ground - 3.0), 1.0).material == Material::Regolith;
        deeper += old_loose && !new_loose ? 1 : 0;
    }
    CHECK(deeper > count / 2);
}
