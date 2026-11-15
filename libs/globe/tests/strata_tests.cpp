#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;

namespace {

const Generator& wet() {
    return hog::testing::small_world();
}

// Too cold and thin-aired for seas.
Generator frozen() {
    hog::globe::Planet planet = hog::testing::small_planet();
    planet.system.air.pressure = 636.0;
    planet.system.stars.front().luminosity = 0.3;
    return Generator{planet};
}

// The share of places that show `material` `depth` metres down, with `layers` everywhere.
double share_of(const hog::globe::terms::Layering& layers, double depth, Material material) {
    const hog::globe::Planet& planet = wet().planet();
    const Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &wet().derived(), .shape = &shape};
    constexpr int count = 20'000;
    int shown = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 down = hog::globe::center(
            hog::globe::cell_at_index(14, (k * 2'654'435'761ULL) % 1'610'612'736ULL));
        const hog::globe::Scatter scatter = hog::globe::terms::scatter_at(context, down, layers);
        shown += hog::globe::terms::material_at(context, layers, scatter, down, depth,
                                                planet.body.radius - depth) == material
                     ? 1
                     : 0;
    }
    return static_cast<double>(shown) / count;
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
    const Generator& earth = wet();
    const std::optional<double> level = earth.sea_level();
    REQUIRE(level);
    const double sea = level.value_or(0.0);
    int soaked = 0;
    int dry = 0;
    for (std::uint64_t k = 0; k < 1'000; ++k) {
        const DVec3 down = direction(k);
        const double ground = hog::globe::ground_below(earth, down, 1.0).value_or(0.0);
        const hog::globe::Sample under = earth.sample(down * (ground - 1.0), 1.0);
        REQUIRE(under.distance < 0.0);
        if (ground < sea) {
            CHECK(under.liquid == Material::Water);
            // Its pores alone where the voxel lies wholly in the ground, not across steep ground's
            // surface a metre over.
            if (under.distance <= -0.25) {
                CHECK(under.water == hog::globe::properties(under.material).porosity);
            }
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

TEST_CASE("softer ground blends further into what it lies on", "[globe][strata]") {
    using hog::globe::terms::contact_width;
    CHECK(contact_width(Material::Sand) == hog::globe::terms::widest_contact);
    CHECK(contact_width(Material::Soil) > contact_width(Material::Clay));
    CHECK(contact_width(Material::Clay) > contact_width(Material::Ice));
    CHECK(contact_width(Material::Ice) > contact_width(Material::Shale));
    CHECK(contact_width(Material::Shale) > contact_width(Material::Granite));
    CHECK(contact_width(Material::Basalt) > 0.0);
}

TEST_CASE("a layer thinning out shows in patches, ever fewer, none where it ends",
          "[globe][strata]") {
    const double width = hog::globe::terms::contact_width(Material::Soil);
    const auto soil = [&](double thickness) {
        return share_of({.cover = thickness, .basement = Material::Granite, .organic = thickness},
                        0.0, Material::Soil);
    };
    // None where it thins to nothing, so no line shows where it ends; its contact wanders up
    // no more than half way, so it shows at half again its share of the width.
    CHECK(soil(0.0) == 0.0);
    CHECK(soil(0.02 * width) < 0.05);
    double last = 0.0;
    for (const double part : {0.2, 0.4, 0.6}) {
        const double share = soil(part * width);
        CHECK(share > last);
        CHECK(std::abs(share - (1.5 * part)) < 0.06);
        last = share;
    }
    CHECK(soil(0.7 * width) == 1.0);
}

TEST_CASE("a contact wanders half its width either way of where it was laid", "[globe][strata]") {
    const double width = std::max(hog::globe::terms::contact_width(Material::Soil),
                                  hog::globe::terms::contact_width(Material::Clay));
    const hog::globe::terms::Layering layers{
        .cover = 10.0, .basement = Material::Granite, .organic = 2.0, .saprolite = 8.0};
    CHECK(share_of(layers, 2.0 - (width / 2.0) - 0.01, Material::Soil) == 1.0);
    const double between = share_of(layers, 2.0, Material::Soil);
    CHECK(between > 0.4);
    CHECK(between < 0.6);
    CHECK(share_of(layers, 2.0 + (width / 2.0) + 0.01, Material::Clay) == 1.0);
}

TEST_CASE("layers of one material lie as one, with no contact between them", "[globe][strata]") {
    // Litter over topsoil, both soil, thick enough together that soil always shows at the top.
    const hog::globe::terms::Layering layers{.cover = 10.0,
                                             .basement = Material::Granite,
                                             .organic = 0.6,
                                             .topsoil = 0.6,
                                             .saprolite = 8.8};
    CHECK(share_of(layers, 0.0, Material::Soil) == 1.0);
}

TEST_CASE("two basements lie mixed in bodies, not in columns running straight down",
          "[globe][strata]") {
    const hog::globe::Planet& planet = wet().planet();
    const Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &wet().derived(), .shape = &shape};
    const hog::globe::terms::Layering basements{
        .basement = Material::Granite, .basement_with = Material::Basalt, .basement_share = 0.3};
    constexpr int count = 5'000;
    int changed = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 down = hog::globe::center(
            hog::globe::cell_at_index(14, (k * 2'654'435'761ULL) % 1'610'612'736ULL));
        const hog::globe::Scatter scatter = hog::globe::terms::scatter_at(context, down, basements);
        const auto at = [&](double depth) {
            return hog::globe::terms::material_at(context, basements, scatter, down, depth,
                                                  planet.body.radius - depth);
        };
        changed += at(5.0) != at(40.0) ? 1 : 0;
    }
    // Picked apart, two depths differ in 2 x 0.3 x 0.7 of the columns.
    CHECK(static_cast<double>(changed) / count > 0.3);
}

TEST_CASE("where two basements or beach materials meet, each shows at its share",
          "[globe][strata]") {
    const hog::globe::terms::Layering basements{
        .basement = Material::Granite, .basement_with = Material::Basalt, .basement_share = 0.3};
    CHECK(std::abs(share_of(basements, 5.0, Material::Basalt) - 0.3) < 0.05);
    CHECK(share_of({.basement = Material::Granite}, 5.0, Material::Granite) == 1.0);
    const hog::globe::terms::Layering beach{.cover = 3.0,
                                            .basement = Material::Granite,
                                            .beach = 3.0,
                                            .beach_of = Material::Sand,
                                            .beach_with = Material::Shingle,
                                            .beach_share = 0.4};
    CHECK(std::abs(share_of(beach, 0.0, Material::Shingle) - 0.4) < 0.05);
    CHECK(std::abs(share_of(beach, 0.0, Material::Sand) - 0.6) < 0.05);
}
