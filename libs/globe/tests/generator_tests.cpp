#include "fine_planet.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

using hog::DVec3;
using hog::globe::Generator;

namespace {

enum class World : std::uint8_t { Wet, Frozen, Airless };

hog::globe::Planet planet_of(World world) {
    hog::globe::Planet planet = fine(hog::globe::Planet{});
    if (world == World::Frozen) {
        planet.system.air.pressure = 636.0;
        planet.system.stars.front().luminosity = 0.3;
    } else if (world == World::Airless) {
        planet.system.air.pressure = 0.0;
    }
    return planet;
}

Generator generator_for(World world) {
    return Generator{planet_of(world)};
}

constexpr std::array worlds{World::Wet, World::Frozen, World::Airless};

// Directions spread over the sphere.
DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(5, (k * 2'654'435'761ULL) % 6'144));
}

struct Census {
    double sea = 0.0; // share under the sea
    double lowest = 1e30;
    double highest = -1e30;
};

Census census(const Generator& generator, int count) {
    const hog::globe::Planet& planet = generator.planet();
    Census found;
    for (int k = 0; k < count; ++k) {
        const DVec3 down = direction(static_cast<std::uint64_t>(k));
        const double ground = hog::globe::ground_below(generator, down, 1.0).value_or(0.0);
        const double height = ground - planet.body.radius;
        found.lowest = std::min(found.lowest, height);
        found.highest = std::max(found.highest, height);
        found.sea += height < 0.0 && generator.sea_level() ? 1.0 / count : 0.0;
    }
    return found;
}

} // namespace

TEST_CASE("a wet world's sea stands at its radius and its ground inside its shell",
          "[globe][generator]") {
    const Generator earth = generator_for(World::Wet);
    const Census found = census(earth, 1500);
    REQUIRE(earth.sea_level().has_value());
    CHECK(earth.sea_level().value_or(0.0) == earth.planet().body.radius);
    CHECK(found.sea > 0.3);
    CHECK(found.sea < 0.95);
    // A shell from the floor under the deepest sea to the sky over the highest ground.
    const double sky = earth.planet().sky_height + std::max(0.0, earth.shape().highest);
    CHECK(earth.layers().ceiling() - earth.planet().body.radius >= sky);
    CHECK(earth.layers().ceiling() - earth.planet().body.radius < sky + earth.layers().thickness);
    UNSCOPED_INFO("ground " << found.lowest << " to " << found.highest);
    CHECK(found.lowest - earth.layers().floor + earth.planet().body.radius >
          0.75 * earth.planet().floor_depth);
    CHECK(found.highest + earth.planet().sky_height <
          earth.layers().ceiling() - earth.planet().body.radius + earth.layers().thickness);
}

TEST_CASE("a world where nothing lies liquid has no sea", "[globe][generator]") {
    for (const World kind : {World::Frozen, World::Airless}) {
        const Generator world = generator_for(kind);
        CHECK_FALSE(world.sea_level().has_value());
        const Census found = census(world, 500);
        CHECK(found.sea == 0.0);
        CHECK(found.lowest - world.layers().floor + world.planet().body.radius >
              0.75 * world.planet().floor_depth);
        CHECK(found.highest + world.planet().sky_height <
              world.layers().ceiling() - world.planet().body.radius + world.layers().thickness);
    }
}

TEST_CASE("distance is a true distance near the ground", "[globe][generator]") {
    // Taken where the ground's finest detail is still wide against the offset and the step, so
    // what's measured is the estimate, not the ground curving under it.
    constexpr double lod = 16.0;
    constexpr double h = 0.25;
    for (const World kind : worlds) {
        const Generator world = generator_for(kind);
        double worst = 0.0;
        double total = 0.0;
        constexpr int count = 300;
        for (std::uint64_t k = 0; k < count; ++k) {
            const DVec3 down = direction(k);
            const double ground = hog::globe::ground_below(world, down, lod).value_or(0.0);
            const DVec3 near = down * (ground + 2.0);
            CHECK(std::abs(world.sample(down * ground, lod).distance) < 1e-3);
            DVec3 slope{0.0};
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                DVec3 step{0.0};
                step[axis] = h;
                slope[axis] = (world.sample(near + step, lod).distance -
                               world.sample(near - step, lod).distance) /
                              (2.0 * h);
            }
            const double error = std::abs(glm::length(slope) - 1.0);
            worst = std::max(worst, error);
            total += error / count;
        }
        UNSCOPED_INFO(static_cast<int>(kind) << " distance slope off by " << total
                                             << " on average, " << worst << " at worst");
        CHECK(total < 0.01);
        // Sharp edges are the worst: the estimate is a slope's, and an edge has two.
        CHECK(worst < 0.08);
    }
}

TEST_CASE("rock warms with depth and air cools with height", "[globe][generator]") {
    const Generator earth = generator_for(World::Wet);
    const DVec3 down = direction(17);
    const double ground = hog::globe::ground_below(earth, down, 1.0).value_or(0.0);
    const double surface = earth.sample(down * (ground + 0.1), 1.0).temperature;
    const double deep = earth.sample(down * (ground - 1000.0), 1.0).temperature;
    CHECK(std::abs(deep - surface - (1000.0 * earth.derived().geotherm)) < 1.0);
    CHECK(earth.sample(down * (earth.planet().body.radius + 3000.0), 1.0).temperature <
          earth.sample(down * (earth.planet().body.radius + 1000.0), 1.0).temperature - 10.0);
    const Generator airless = generator_for(World::Airless);
    CHECK(airless.sample(down * (airless.planet().body.radius + 3000.0), 1.0).temperature ==
          airless.sample(down * (airless.planet().body.radius + 1000.0), 1.0).temperature);
}

TEST_CASE("the same planet is generated the same, bit for bit", "[globe][generator]") {
    std::uint64_t digest = 0;
    for (const World kind : worlds) {
        const Generator world = generator_for(kind);
        for (std::uint64_t k = 0; k < 400; ++k) {
            const double radius =
                world.planet().body.radius + (static_cast<double>(k % 40) * 250.0) - 5'000.0;
            const hog::globe::Sample sample = world.sample(direction(k) * radius, 0.5);
            digest = hog::hash_values(digest, std::bit_cast<std::uint64_t>(sample.distance),
                                      static_cast<int>(sample.material),
                                      std::bit_cast<std::uint64_t>(sample.water),
                                      std::bit_cast<std::uint64_t>(sample.temperature));
        }
    }
    // If this moves, unchanged ground no longer matches what was there before, or elsewhere.
    CHECK(digest == 0xf637'fc8d'7985'ebf9ULL);
}

TEST_CASE("generator throughput", "[.bench]") {
    // Lone samples pay for their column each time; a chunk shares one column across its
    // layers, as meshing will.
    for (const World kind : worlds) {
        const Generator world = generator_for(kind);
        const double radius = world.planet().body.radius;
        constexpr std::uint64_t columns = 2'000;
        constexpr int layers = 64;
        const auto started = std::chrono::steady_clock::now();
        double sink = 0.0;
        for (std::uint64_t k = 0; k < columns; ++k) {
            sink += world.sample(direction(k) * radius, 0.5).distance;
        }
        const auto lone = std::chrono::steady_clock::now();
        for (std::uint64_t k = 0; k < columns; ++k) {
            const Generator::Column column = world.column(direction(k), 0.5);
            for (int layer = 0; layer < layers; ++layer) {
                sink += column.sample(radius - 16.0 + (layer * 0.5)).distance;
            }
        }
        const auto chunked = std::chrono::steady_clock::now();
        const auto seconds = [](auto from, auto to) {
            return std::chrono::duration<double>(to - from).count();
        };
        WARN(static_cast<int>(kind)
             << ": " << columns / seconds(started, lone) << " lone samples/s, "
             << (columns * layers) / seconds(lone, chunked) << " samples/s a column at a time ("
             << sink << ")");
    }
}

TEST_CASE("land is found near a direction, over the sea, and anywhere on a dry world",
          "[globe][generator]") {
    const Generator earth = generator_for(World::Wet);
    const DVec3 start{1.0, 0.0, 0.0};
    const DVec3 land = hog::globe::land_near(earth, start).value_or(DVec3{0.0});
    REQUIRE(land != DVec3{0.0});
    CHECK(hog::globe::ground_below(earth, land, hog::globe::voxel_lod(earth)).value_or(0.0) >
          earth.sea_level().value_or(0.0));
    // Nearer than a few rings, or it would have been found sooner.
    CHECK(glm::length(land - start) * earth.planet().body.radius < 100'000.0);
    // Within nothing, only the direction itself can be it.
    const bool start_is_land =
        hog::globe::ground_below(earth, start, hog::globe::voxel_lod(earth) * 16.0).value_or(0.0) >
        earth.sea_level().value_or(0.0);
    CHECK(hog::globe::land_near(earth, start, 0.0).has_value() == start_is_land);
    const Generator dry = generator_for(World::Airless);
    CHECK(hog::globe::land_near(dry, start) == glm::normalize(start));
}

TEST_CASE("the highest ground formed bounds the ground", "[globe][generator]") {
    const Generator world = generator_for(World::Wet);
    CHECK(world.shape().highest > 100.0);
    CHECK(world.shape().highest > world.shape().lowest);
    for (std::uint64_t k = 0; k < 300; ++k) {
        const double ground = hog::globe::ground_below(world, direction(k), 0.5).value_or(0.0) -
                              world.planet().body.radius;
        CHECK(ground < (world.shape().highest * 1.25) + 50.0);
    }
}
