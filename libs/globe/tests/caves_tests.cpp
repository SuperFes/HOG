#include "../src/terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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

// Ground of one rock all the way down, under the planet's radius.
hog::globe::terms::Layering all_of(Material rock) {
    return {.basement = rock};
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
        const double below = depth - static_cast<double>(k % 7);
        open +=
            hog::globe::terms::cave_distance(context, all_of(rock), direction(k),
                                             planet.radius - below, planet.radius, below, 0.5) < 0.0
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

TEST_CASE("caves keep to their beds, their walls running on across the beds' faces",
          "[globe][caves]") {
    // Down a column the distance never leaps from one step to the next, even where one bed gives
    // way to another and a cave in the one stops at its face: a step that changes it far more
    // than the steps either side is a leap.
    const Generator earth{named("earth")};
    constexpr double step = 0.05;
    int leaps = 0;
    int open = 0;
    for (std::uint64_t k = 0; k < 400; ++k) {
        const Generator::Column column = earth.column(direction(k), 0.5);
        std::vector<double> down_it;
        for (int down = 0; down < 1'200; ++down) {
            down_it.push_back(column.sample(column.ground() - (down * step)).distance);
            open += down_it.back() > 0.0 ? 1 : 0;
        }
        for (std::size_t at = 2; at + 1 < down_it.size(); ++at) {
            const double before = std::abs(down_it[at - 1] - down_it[at - 2]);
            const double change = std::abs(down_it[at] - down_it[at - 1]);
            const double after = std::abs(down_it[at + 1] - down_it[at]);
            const bool near = std::abs(down_it[at]) < 5.0 || std::abs(down_it[at - 1]) < 5.0;
            leaps += near && change > 0.25 && change > 4.0 * std::max(before, after) ? 1 : 0;
        }
    }
    UNSCOPED_INFO(leaps << " leaps by " << open << " open points");
    CHECK(open > 0);
    CHECK(leaps == 0);
}

TEST_CASE("chambers shrink away under thin rock, their roofs fallen in", "[globe][caves]") {
    const Planet earth = named("earth");
    const hog::globe::Derived derived = hog::globe::derive(earth);
    const Generator::Shape shape{.sea = true};
    const hog::globe::terms::Context context{
        .planet = &earth, .derived = &derived, .shape = &shape};
    // Under as much rock as a bed spans and more, then under a few metres; same depth either way.
    // Points further into open space than any passage is wide lie in chambers.
    const auto open_under = [&](double cover) {
        int open = 0;
        for (std::uint64_t k = 0; k < 40'000; ++k) {
            open += hog::globe::terms::cave_distance(context, all_of(Material::Limestone),
                                                     direction(k), earth.radius - 60.0,
                                                     earth.radius, cover, 0.5) < -4.0
                        ? 1
                        : 0;
        }
        return open;
    };
    const int deep = open_under(500.0);
    const int shallow = open_under(4.0);
    UNSCOPED_INFO("open under 500 m: " << deep << ", under 4 m: " << shallow);
    CHECK(deep > 0);
    CHECK(shallow < deep / 10);
}
