#include "fine_planet.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;

namespace {

Generator generator_for(std::string_view name) {
    return Generator{fine(hog::globe::preset(name).value_or(hog::globe::Planet{}))};
}

// Directions spread over the sphere, apart from the generator's own survey.
DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(5, (k * 2'654'435'761ULL) % 6'144));
}

struct Census {
    double high = 0.0; // share of the surface on high crust
    double sea = 0.0;  // share under the sea
    double mean = 0.0; // mean ground, metres over the radius
    double lowest = 1e30;
    double highest = -1e30;
    DVec3 high_centre{0.0};
    DVec3 figure{0.0}; // the centre of the ground's figure, off the centre of the planet
};

Census census(const Generator& generator, int count) {
    const hog::globe::Planet& planet = generator.planet();
    Census found;
    for (int k = 0; k < count; ++k) {
        const DVec3 down = direction(static_cast<std::uint64_t>(k));
        const double ground = hog::globe::ground_below(generator, down, 1.0).value_or(0.0);
        const double height = ground - planet.radius;
        found.mean += height / count;
        found.lowest = std::min(found.lowest, height);
        found.highest = std::max(found.highest, height);
        found.figure += down * (height / count);
        found.sea += height < 0.0 && generator.sea_level() ? 1.0 / count : 0.0;
        // Under the soil and the beds, the crust's own rock.
        if (generator.sample(down * (ground - 2'500.0), 1.0).material == planet.high.rock &&
            planet.high.rock != planet.low.rock) {
            found.high += 1.0 / count;
            found.high_centre += down / static_cast<double>(count);
        }
    }
    return found;
}

} // namespace

TEST_CASE("the crust and the sea cover their shares of the surface", "[globe][generator]") {
    const Generator earth = generator_for("earth");
    const Census found = census(earth, 3000);
    CHECK(std::abs(found.high - 0.4) < 0.04);
    CHECK(std::abs(found.sea - 0.7) < 0.04);
    REQUIRE(earth.sea_level().has_value());
    CHECK(earth.sea_level().value_or(0.0) == earth.planet().radius);
    // Scattered continents: no side holds most of them.
    CHECK(glm::length(found.high_centre) < 0.1);
    CHECK(glm::length(found.figure) < 0.1 * earth.derived().isostatic_step);
    // A shell from the floor under the deepest sea to the sky.
    CHECK(earth.layers().ceiling() - earth.planet().radius == earth.planet().sky_height);
    CHECK(earth.layers().count > 28'000);
    UNSCOPED_INFO("earth ground " << found.lowest << " to " << found.highest);
    // The floor is placed under the ground surveyed at kilometre scale; finer detail cuts a
    // little deeper, never most of the way.
    CHECK(found.lowest - earth.layers().floor + earth.planet().radius >
          0.75 * earth.planet().floor_depth);
    CHECK(found.highest < earth.planet().sky_height);
}

TEST_CASE("dry worlds stand at their mean ground, Mars's highlands to one side",
          "[globe][generator]") {
    for (const std::string_view name : {"mars", "venus", "moon"}) {
        const Generator world = generator_for(name);
        CHECK_FALSE(world.sea_level().has_value());
        const Census found = census(world, 1500);
        UNSCOPED_INFO(name << " ground " << found.lowest << " to " << found.highest << ", mean "
                           << found.mean << ", high " << found.high);
        CHECK(std::abs(found.mean) < 300.0);
        CHECK(found.sea == 0.0);
        CHECK(found.lowest - world.layers().floor + world.planet().radius >
              0.75 * world.planet().floor_depth);
        CHECK(found.highest < world.planet().sky_height);
        // Mars's figure stands kilometres off its centre, toward the southern highlands.
        const double offset = glm::length(found.figure);
        if (name == "mars") {
            CHECK(offset > 0.15 * world.derived().isostatic_step);
        } else if (name == "venus") {
            CHECK(offset < 0.02 * world.derived().tallest);
        }
    }
}

TEST_CASE("distance is a true distance near the ground", "[globe][generator]") {
    // Taken where the ground's finest detail is still wide against the offset and the step, so
    // what's measured is the estimate, not the ground curving under it.
    constexpr double lod = 16.0;
    constexpr double h = 0.25;
    for (const std::string_view name : {"earth", "mars", "venus", "moon"}) {
        const Generator world = generator_for(name);
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
        UNSCOPED_INFO(name << " distance slope off by " << total << " on average, " << worst
                           << " at worst");
        CHECK(total < 0.01);
        // Sharp crater rims are the worst: the estimate is a slope's, and a rim has two.
        CHECK(worst < 0.08);
    }
}

TEST_CASE("rock warms with depth and air cools with height", "[globe][generator]") {
    const Generator earth = generator_for("earth");
    const DVec3 down = direction(17);
    const double ground = hog::globe::ground_below(earth, down, 1.0).value_or(0.0);
    const double surface = earth.sample(down * (ground + 0.1), 1.0).temperature;
    const double deep = earth.sample(down * (ground - 1000.0), 1.0).temperature;
    CHECK(std::abs(deep - surface - (1000.0 * earth.derived().geotherm)) < 1.0);
    CHECK(earth.sample(down * (earth.planet().radius + 3000.0), 1.0).temperature <
          earth.sample(down * (earth.planet().radius + 1000.0), 1.0).temperature - 10.0);
    const Generator moon = generator_for("moon");
    CHECK(moon.sample(down * (moon.planet().radius + 3000.0), 1.0).temperature ==
          moon.sample(down * (moon.planet().radius + 1000.0), 1.0).temperature);
}

TEST_CASE("the same planet is generated the same, bit for bit", "[globe][generator]") {
    std::uint64_t digest = 0;
    for (const std::string_view name : {"earth", "mars", "venus", "moon"}) {
        const Generator world = generator_for(name);
        for (std::uint64_t k = 0; k < 400; ++k) {
            const double radius =
                world.planet().radius + (static_cast<double>(k % 40) * 250.0) - 5'000.0;
            const hog::globe::Sample sample = world.sample(direction(k) * radius, 0.5);
            digest = hog::hash_values(digest, std::bit_cast<std::uint64_t>(sample.distance),
                                      static_cast<int>(sample.material),
                                      std::bit_cast<std::uint64_t>(sample.water),
                                      std::bit_cast<std::uint64_t>(sample.temperature));
        }
    }
    // If this moves, unchanged ground no longer matches what was there before, or elsewhere.
    CHECK(digest == 0x866b'b163'877d'8e86ULL);
}

TEST_CASE("generator throughput", "[.bench]") {
    // Lone samples pay for their column each time; a chunk shares one column across its
    // layers, as meshing will.
    for (const std::string_view name : {"earth", "mars", "venus", "moon"}) {
        const Generator world = generator_for(name);
        const double radius = world.planet().radius;
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
        WARN(name << ": " << columns / seconds(started, lone) << " lone samples/s, "
                  << (columns * layers) / seconds(lone, chunked)
                  << " samples/s a column at a time (" << sink << ")");
    }
}

TEST_CASE("a sample's normal is the way its distance grows", "[globe][generator]") {
    for (const std::string_view name : {"earth", "mars", "venus", "moon"}) {
        const Generator world = generator_for(name);
        constexpr double lod = 1.0;
        constexpr double step = 0.02;
        int near_surface = 0;
        int agreeing = 0;
        for (std::uint64_t k = 0; k < 400; ++k) {
            const DVec3 down = direction(k);
            const double ground = hog::globe::ground_below(world, down, lod).value_or(0.0);
            // At the surface: off it, the distance's own rate bends with the slope's curvature.
            for (const double off : {-0.1, -0.03, 0.03, 0.1}) {
                const DVec3 point = down * (ground + off);
                const hog::globe::Sample sample = world.sample(point, lod);
                DVec3 rise{0.0};
                for (glm::length_t axis = 0; axis < 3; ++axis) {
                    DVec3 along{0.0};
                    along[axis] = step;
                    rise[axis] = (world.sample(point + along, lod).distance -
                                  world.sample(point - along, lod).distance) /
                                 (2.0 * step);
                }
                if (!(glm::length(rise) > 0.0)) {
                    continue;
                }
                ++near_surface;
                CHECK(std::abs(glm::length(sample.normal) - 1.0) < 1e-9);
                agreeing += glm::dot(sample.normal, glm::normalize(rise)) > std::cos(0.1) ? 1 : 0;
            }
        }
        CAPTURE(name, near_surface, agreeing);
        CHECK(near_surface > 1'000);
        CHECK(agreeing > near_surface * 95 / 100);
    }
}

TEST_CASE("a cave wall's normal is the way its distance grows", "[globe][generator]") {
    const Generator world = generator_for("earth");
    constexpr double lod = 1.0;
    constexpr double step = 0.01;
    const auto distance_at = [&](DVec3 point) { return world.sample(point, lod).distance; };
    int on_walls = 0;
    int agreeing = 0;
    for (std::uint64_t k = 0; k < 20'000 && on_walls < 500; ++k) {
        const DVec3 down = direction(k);
        const auto column = world.column(down, lod);
        // Deep enough that any surface is a cave's, as the ground's blend reaches a metre.
        for (int quarter = 12; quarter < 240; ++quarter) {
            const double depth = quarter / 4.0;
            const hog::globe::Sample sample = column.sample(column.ground() - depth);
            if (std::abs(sample.distance) > 0.1) {
                continue;
            }
            const DVec3 point = down * (column.ground() - depth);
            DVec3 rise{0.0};
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                DVec3 along{0.0};
                along[axis] = step;
                rise[axis] =
                    (distance_at(point + along) - distance_at(point - along)) / (2.0 * step);
            }
            ++on_walls;
            agreeing +=
                glm::dot(world.sample(point, lod).normal, glm::normalize(rise)) > std::cos(0.1) ? 1
                                                                                                : 0;
        }
    }
    CAPTURE(on_walls, agreeing);
    CHECK(on_walls > 100);
    CHECK(agreeing > on_walls * 95 / 100);
}

TEST_CASE("land is found near a direction, over the sea, and anywhere on a dry world",
          "[globe][generator]") {
    const Generator earth = generator_for("earth");
    const DVec3 start{1.0, 0.0, 0.0};
    const DVec3 land = hog::globe::land_near(earth, start).value_or(DVec3{0.0});
    REQUIRE(land != DVec3{0.0});
    CHECK(hog::globe::ground_below(earth, land, hog::globe::voxel_lod(earth)).value_or(0.0) >
          earth.sea_level().value_or(0.0));
    // Nearer than a few rings, or it would have been found sooner.
    CHECK(glm::length(land - start) * earth.planet().radius < 100'000.0);
    // Within nothing, only the direction itself can be it.
    const bool start_is_land =
        hog::globe::ground_below(earth, start, hog::globe::voxel_lod(earth) * 16.0).value_or(0.0) >
        earth.sea_level().value_or(0.0);
    CHECK(hog::globe::land_near(earth, start, 0.0).has_value() == start_is_land);
    const Generator moon = generator_for("moon");
    CHECK(hog::globe::land_near(moon, start) == glm::normalize(start));
}

TEST_CASE("the highest ground surveyed bounds the ground", "[globe][generator]") {
    const Generator world = generator_for("earth");
    CHECK(world.shape().highest > 100.0);
    CHECK(world.shape().highest > world.shape().lowest);
    for (std::uint64_t k = 0; k < 300; ++k) {
        const double ground = hog::globe::ground_below(world, direction(k), 0.5).value_or(0.0) -
                              world.planet().radius;
        CHECK(ground < (world.shape().highest * 1.25) + 50.0);
    }
}

TEST_CASE("running water wears the land and leaves the sea and the world's size",
          "[globe][generator]") {
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    const Generator dry{planet};
    planet.wear = 0.03;
    const Generator worn{planet};
    REQUIRE(!worn.shape().cut.empty());
    CHECK(dry.shape().cut.empty());
    const auto [least, most] = std::ranges::minmax(worn.shape().cut);
    CHECK(least >= 0.0F);
    CHECK(most > 1.0F);
    double lowered = 0.0;
    double raised = 0.0;
    for (std::uint64_t k = 0; k < 400; ++k) {
        const DVec3 up = hog::globe::center(hog::globe::cell_at_index(5, k * 13));
        const double change = worn.column(up, 200.0).ground() - dry.column(up, 200.0).ground();
        lowered = std::min(lowered, change);
        raised = std::max(raised, change);
    }
    CHECK(lowered < -1.0);
    CHECK(raised < 1e-6);
}

TEST_CASE("a hollow the worn ground leaves holds a lake standing level over its ground",
          "[globe][generator]") {
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.wear = 0.03;
    const Generator world{planet};
    const auto& shape = world.shape();
    std::size_t lakes = 0;
    for (std::size_t cell = 0; cell < shape.lake.size(); ++cell) {
        if (shape.lake[cell] <= Generator::Shape::no_lake) {
            continue;
        }
        ++lakes;
        const DVec3 up = hog::globe::center(hog::globe::cell_at_index(shape.cut_level, cell));
        const auto found = world.gathered(up);
        REQUIRE(found.lake);
        const double lake = found.lake.value_or(0.0);
        CHECK(lake >= planet.radius + static_cast<double>(shape.lake[cell]) - 1e-6);
        CHECK(found.catchment > 0.0);
        const auto column = world.column(up, 100.0);
        if (column.ground() < lake - 1.0) {
            const auto sample = column.sample(lake - 0.5);
            CHECK(sample.water > 0.0);
            CHECK(column.sample(lake + 5.0).water == 0.0);
        }
    }
    CHECK(world.gathered(DVec3{0.0, 0.0, 1.0}).catchment > 0.0);
    CHECK(Generator{[&] {
              auto dry = planet;
              dry.wear = 0.0;
              return dry;
          }()}
              .gathered(DVec3{1.0, 0.0, 0.0})
              .catchment == 0.0);
    CHECK(lakes > 0);
}
