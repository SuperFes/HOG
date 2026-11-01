#include "../src/terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Planet;

namespace {

Planet named(std::string_view name) {
    return hog::globe::preset(name).value_or(Planet{});
}

DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(12, (k * 2'654'435'761ULL) % 100'663'296));
}

// The share of points `depth` metres down in `rock` that lie in a cave.
double open_share(const Planet& planet, Material rock, double depth) {
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    constexpr int count = 40'000;
    int open = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        const double radius = planet.radius - depth + static_cast<double>(k % 7);
        open +=
            hog::globe::terms::cave_distance(context, direction(k), radius, rock, depth, 0.5) < 0.0
                ? 1
                : 0;
    }
    return static_cast<double>(open) / count;
}

} // namespace

TEST_CASE("caves open in limestone far more than in other rock", "[globe][caves]") {
    const Planet earth = named("earth");
    const double limestone = open_share(earth, Material::Limestone, 50.0);
    const double shale = open_share(earth, Material::Shale, 50.0);
    const double granite = open_share(earth, Material::Granite, 50.0);
    UNSCOPED_INFO("open: limestone " << limestone << ", shale " << shale << ", granite "
                                     << granite);
    CHECK(limestone > 0.01);
    CHECK(limestone > 3.0 * shale);
    CHECK(shale > granite);
    CHECK(open_share(earth, Material::Soil, 50.0) == 0.0);
    // Dissolution needs moving water: little a kilometre and a half down.
    CHECK(open_share(earth, Material::Limestone, 1'500.0) < 0.25 * limestone);
    // Without water, no karst; but lava tubes run under any world's basalt flows.
    const Planet moon = named("moon");
    CHECK(open_share(moon, Material::Limestone, 50.0) == 0.0);
    CHECK(open_share(moon, Material::Basalt, 15.0) > 0.0);
    CHECK(open_share(moon, Material::Basalt, 200.0) == 0.0);
}

TEST_CASE("some columns cross solid ground more than once", "[globe][caves]") {
    // Where a cave runs under the ground, a column passes rock, open space, then rock again:
    // what a height map can't hold.
    const Generator earth{named("earth")};
    std::uint64_t crossed_again = 0;
    constexpr std::uint64_t columns = 4'000;
    for (std::uint64_t k = 0; k < columns; ++k) {
        const DVec3 down = direction(k);
        const Generator::Column column = earth.column(down, 0.5);
        const double ground = hog::globe::ground_below(earth, down, 0.5).value_or(0.0);
        bool solid = true;
        int entries = 0;
        for (int step = 1; step < 600; ++step) {
            const bool now = column.sample(ground - (step * 0.5)).distance < 0.0;
            entries += now && !solid ? 1 : 0;
            solid = now;
        }
        crossed_again += entries > 0 ? 1 : 0;
    }
    UNSCOPED_INFO(crossed_again << " of " << columns << " columns cross the ground again");
    CHECK(crossed_again > 0);
    CHECK(crossed_again < columns / 4);
}
