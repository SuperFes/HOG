#include "../src/terms.hpp"
#include "fine_planet.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>
#include <hog/globe/planet.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::CavePiece;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Planet;

namespace {

Planet wet() {
    return hog::testing::small_planet();
}

// No air or water, under a sixth of the gravity.
Planet airless() {
    Planet planet = hog::testing::small_planet();
    planet.body.gravity = 1.62;
    planet.system.air.pressure = 0.0;
    return planet;
}

DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(12, (k * 2'654'435'761ULL) % 100'663'296));
}

// Ground of one rock all the way down, under the planet's radius.
hog::globe::terms::Layering all_of(Material rock) {
    return {.basement = rock};
}

// Level ground at the planet's radius over `layers`.
hog::globe::terms::GroundUnder level_over(const Planet& planet,
                                          const hog::globe::terms::Layering& layers) {
    return [radius = planet.body.radius, layers](DVec3) {
        return hog::globe::terms::CaveGround{.ground = radius, .layers = layers};
    };
}

// The caves of a few hundred cave cells spread over the planet, laid out under level ground.
std::vector<std::vector<CavePiece>> caves_under(const Planet& planet,
                                                const hog::globe::terms::Layering& layers) {
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const Generator::Shape shape{.sea = true};
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    const int level = hog::globe::terms::cave_level(planet);
    std::vector<hog::globe::Cell> cells;
    for (std::uint64_t k = 0; cells.size() < 300 && k < 3'000; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_under(direction(k), level);
        if (std::ranges::find(cells, cell) == cells.end()) {
            cells.push_back(cell);
        }
    }
    std::vector<std::vector<CavePiece>> laid;
    laid.reserve(cells.size());
    const hog::globe::terms::GroundUnder under = level_over(planet, layers);
    for (const hog::globe::Cell cell : cells) {
        laid.push_back(hog::globe::terms::lay_out_caves(context, cell, under));
    }
    return laid;
}

// Cubic metres open in a cave cell on average, within 25 m of `depth` metres down under level
// ground over `layers`.
double open_volume(const Planet& planet, const hog::globe::terms::Layering& layers, double depth) {
    double volume = 0.0;
    const std::vector<std::vector<CavePiece>> laid = caves_under(planet, layers);
    for (const std::vector<CavePiece>& pieces : laid) {
        for (const CavePiece& piece : pieces) {
            const double down = planet.body.radius - glm::length((piece.from + piece.to) * 0.5);
            if (std::abs(down - depth) > 25.0) {
                continue;
            }
            const double r = piece.radius;
            volume += (std::numbers::pi * r * r * glm::length(piece.to - piece.from)) +
                      (4.0 / 3.0 * std::numbers::pi * r * r * r);
        }
    }
    return volume / static_cast<double>(laid.size());
}

// The same in `rock` all the way down.
double open_volume(const Planet& planet, Material rock, double depth) {
    return open_volume(planet, all_of(rock), depth);
}

double floor_of(const Planet& planet) {
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const Generator::Shape shape;
    return hog::globe::terms::cave_floor({.planet = &planet, .derived = &derived, .shape = &shape});
}

} // namespace

TEST_CASE("a column's caves are found by casting down it, wherever its samples are open",
          "[globe][caves]") {
    const Generator& generator = hog::testing::small_world();
    const double lod = hog::globe::voxel_lod(generator);
    constexpr double reach = 300.0;
    int with = 0;
    for (std::uint64_t k = 0; k < 600 && with < 12; ++k) {
        const Generator::Column column = generator.column(direction(k), lod);
        const double lowest = column.ground() - reach;
        const std::vector<std::pair<double, double>> caves =
            column.caves(lowest, column.ground(), 0.0);
        INFO("column " << k << " with " << caves.size() << " caves");
        for (int step = 1; static_cast<double>(step) * 0.5 < reach; ++step) {
            const double radius = column.ground() - (static_cast<double>(step) * 0.5);
            if (column.sample(radius).distance <= 0.0) {
                continue;
            }
            // Open under the ground: in a cave found, which holds it.
            bool found = false;
            double nearest = 1e9;
            for (const auto& [bottom, top] : caves) {
                found = found || (radius >= bottom && radius <= top);
                nearest = std::min({nearest, std::abs(radius - bottom), std::abs(radius - top)});
            }
            INFO(column.ground() - radius << " m down, open by " << column.sample(radius).distance
                                          << ", nearest cave edge " << nearest << " m off");
            CHECK(found);
        }
        for (std::size_t c = 0; c < caves.size(); ++c) {
            CHECK(caves[c].first < caves[c].second);
            if (c > 0) {
                CHECK(caves[c - 1].second < caves[c].first);
            }
        }
        with += caves.empty() ? 0 : 1;
    }
    CHECK(with >= 6);
}

TEST_CASE("asked about far wider than caves are gathered a column answers from depth alone",
          "[globe][caves]") {
    const Generator& generator = hog::testing::small_world();
    const double lod = hog::globe::voxel_lod(generator);
    const double radius = generator.planet().body.radius;
    // As wide as a coarse node, kilometres: answered at once, laying out no cave cells.
    constexpr double wide = 20'000.0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t k = 0; k < 200; ++k) {
        const Generator::Column column = generator.column(direction(k), lod);
        const double ground = column.ground();
        INFO("column " << k);
        // Anywhere a cave near shows, the wide answer has one too.
        for (const double down : {5.0, 40.0, 150.0}) {
            if (column.caves_may_lie(ground - down - 10.0, ground - down, 200.0)) {
                CHECK(column.caves_may_lie(ground - down - 10.0, ground - down, wide));
                CHECK_FALSE(column.caves(ground - down - 10.0, ground - down, wide).empty());
            }
        }
        // None over the highest ground nor deeper than any cave opens.
        const double above = radius + generator.shape().highest + 100.0;
        CHECK_FALSE(column.caves_may_lie(above, above + 10.0, wide));
        CHECK_FALSE(column.caves_may_lie(radius - 20'000.0, radius - 19'990.0, wide));
    }
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 30.0);
}

TEST_CASE("the cost of laying out a cave cell, and of a column among its pieces",
          "[.bench][globe][caves]") {
    const Planet earth = wet();
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::vector<CavePiece>> laid =
        caves_under(earth, all_of(Material::Limestone));
    const auto took = std::chrono::steady_clock::now() - started;
    std::size_t pieces = 0;
    for (const std::vector<CavePiece>& each : laid) {
        pieces += each.size();
    }
    std::println("laying out: {} us a cell, {} pieces a cell",
                 std::chrono::duration<double, std::micro>(took).count() /
                     static_cast<double>(laid.size()),
                 static_cast<double>(pieces) / static_cast<double>(laid.size()));
    const Generator& generator = hog::testing::small_world();
    constexpr int columns = 2'000;
    double sum = 0.0;
    const auto sampled = std::chrono::steady_clock::now();
    for (std::uint64_t k = 0; k < columns; ++k) {
        const Generator::Column column = generator.column(direction(k), 0.5);
        for (int step = 0; step < 100; ++step) {
            sum += column.sample(column.ground() - (static_cast<double>(step) * 2.0)).distance;
        }
    }
    std::println(
        "a column and 100 samples down it: {} us ({})",
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - sampled)
                .count() /
            columns,
        sum);
}

TEST_CASE("caves open in limestone far more than in other rock", "[globe][caves]") {
    const Planet earth = wet();
    const double limestone = open_volume(earth, Material::Limestone, 50.0);
    const double shale = open_volume(earth, Material::Shale, 50.0);
    const double granite = open_volume(earth, Material::Granite, 50.0);
    UNSCOPED_INFO("open: limestone " << limestone << " m3, shale " << shale << ", granite "
                                     << granite);
    CHECK(limestone > 100.0);
    CHECK(limestone > 3.0 * shale);
    CHECK(shale > granite);
    CHECK(open_volume(earth, Material::Soil, 50.0) == 0.0);
    // Dissolution needs moving water: little a kilometre and a half down.
    CHECK(open_volume(earth, Material::Limestone, 1'500.0) < 0.25 * limestone);
    // Without water, no karst; but lava tubes run under any world's basalt flows.
    const Planet moon = airless();
    CHECK(open_volume(moon, Material::Limestone, 50.0) == 0.0);
    CHECK(open_volume(moon, Material::Basalt, 15.0) > 0.0);
    CHECK(open_volume(moon, Material::Basalt, 200.0) == 0.0);
}

TEST_CASE("where two basements meet, either's caves open, the other's fading with its share",
          "[globe][caves]") {
    const Planet earth = wet();
    const double granite = open_volume(earth, Material::Granite, 50.0);
    const double limestone = open_volume(earth, Material::Limestone, 50.0);
    const auto mixed = [&](double share) {
        return open_volume(earth,
                           {.basement = Material::Granite,
                            .basement_with = Material::Limestone,
                            .basement_share = share},
                           50.0);
    };
    CHECK(mixed(0.0) == granite);
    const double little = mixed(0.1);
    CHECK(little > granite);
    CHECK(little < limestone);
    CHECK(mixed(0.5) >= limestone);
}

TEST_CASE("some columns cross solid ground more than once", "[globe][caves]") {
    // Where a cave runs under the ground, a column passes rock, open space, then rock again:
    // what a height map can't hold.
    const Generator& earth = hog::testing::small_world();
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
    // Down a column the distance never leaps from one step to the next near the surface it
    // bounds, even where one bed gives way to another and a cave in the one stops at its face: a
    // step that changes it far more than the steps either side is a leap. Deeper in the rock a
    // passage narrowing to nothing may drop away whole, unseen.
    const Generator& earth = hog::testing::small_world();
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
            const bool near = std::abs(down_it[at]) < 1.0 || std::abs(down_it[at - 1]) < 1.0;
            leaps += near && change > 0.25 && change > 4.0 * std::max(before, after) ? 1 : 0;
        }
    }
    UNSCOPED_INFO(leaps << " leaps by " << open << " open points");
    CHECK(open > 0);
    CHECK(leaps == 0);
}

TEST_CASE("limestone opens in passages and chambers right up to open air", "[globe][caves]") {
    const Planet earth = wet();
    const double radius = earth.body.radius;
    int out = 0;
    int chambers = 0;
    int shallow_chambers = 0;
    int passages = 0;
    for (const std::vector<CavePiece>& pieces : caves_under(earth, all_of(Material::Limestone))) {
        for (const CavePiece& piece : pieces) {
            const double top =
                std::max(glm::length(piece.from), glm::length(piece.to)) + piece.radius;
            out += top > radius ? 1 : 0;
            if (piece.from == piece.to) {
                ++chambers;
                shallow_chambers += radius - glm::length(piece.from) < 30.0 ? 1 : 0;
            } else {
                ++passages;
            }
        }
    }
    UNSCOPED_INFO(passages << " stretches of passage, " << chambers << " chambers, "
                           << shallow_chambers << " of them shallow, " << out
                           << " pieces breaking out");
    CHECK(passages > 1'000);
    CHECK(chambers > 10);
    CHECK(chambers < passages / 4);
    CHECK(shallow_chambers > 0);
    CHECK(out > 0);
}

TEST_CASE("a cell's caves are the same laid out again, and keep to the cells beside it",
          "[globe][caves]") {
    const Generator& generator = hog::testing::small_world();
    const Planet& planet = generator.planet();
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const Generator::Shape& shape = generator.shape();
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    const int level = hog::globe::terms::cave_level(planet);
    int laid = 0;
    for (std::uint64_t k = 0; k < 40; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_under(direction(k), level);
        const auto first = hog::globe::terms::caves_in(context, cell);
        const auto again = hog::globe::terms::caves_in(context, cell);
        CHECK(*first == *again);
        laid += first->empty() ? 0 : 1;
        // Every piece lies over cells beside its own.
        for (const CavePiece& piece : *first) {
            const hog::globe::Cell under = hog::globe::cell_under(glm::normalize(piece.to), level);
            const hog::globe::CellNeighbors beside = hog::globe::around(cell);
            CHECK((under == cell || std::ranges::find(beside.all(), under) != beside.all().end()));
        }
    }
    CHECK(laid > 0);
}

TEST_CASE("passages close where the stress round them overcomes the rock, deeper in weaker gravity",
          "[globe][caves]") {
    const double earth = floor_of(wet());
    const double moon = floor_of(airless());
    // Hundreds of metres, kilometres under a sixth of the gravity.
    CHECK(earth > 200.0);
    CHECK(earth < 1'500.0);
    CHECK(moon > 3.0 * earth);
    Planet heavy = wet();
    heavy.body.gravity *= 2.0;
    CHECK(floor_of(heavy) == Catch::Approx(earth / 2.0).margin(60.0));
    // Nothing is open at the floor, and a cast down a column never reaches below it.
    const Generator& generator = hog::testing::small_world();
    for (std::uint64_t k = 0; k < 200; ++k) {
        const Generator::Column column = generator.column(direction(k), 0.5);
        for (const auto& [bottom, top] : column.caves(0.0, 1e9, 0.0)) {
            CHECK(bottom >= column.ground() - earth - 1.0);
        }
    }
}

TEST_CASE("a cast flags little where the rock holds only fine fractures", "[globe][caves]") {
    // The Moon's crystalline rock opens fissures narrower than a voxel, which aren't drawn.
    const Generator generator{airless()};
    double flagged = 0.0;
    for (std::uint64_t k = 0; k < 300; ++k) {
        for (const auto& [bottom, top] :
             generator.column(direction(k), 0.5).caves(0.0, 1e9, 10.0)) {
            flagged += top - bottom;
        }
    }
    CHECK(flagged / 300.0 < 60.0);
}
