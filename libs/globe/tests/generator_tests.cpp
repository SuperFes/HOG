#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Material;

namespace {

Generator generator_for(std::string_view name) {
    return Generator{hog::globe::preset(name).value_or(hog::globe::Planet{})};
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
        if (generator.sample(down * (ground - 1.0), 1.0).material == planet.high.rock &&
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
    CHECK(found.lowest - earth.layers().floor + earth.planet().radius >
          earth.planet().floor_depth - 300.0);
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
              world.planet().floor_depth - 300.0);
        CHECK(found.highest < world.planet().sky_height);
        // Mars's figure stands kilometres off its centre, toward the southern highlands.
        const double offset = glm::length(found.figure);
        if (name == "mars") {
            CHECK(offset > 0.15 * world.derived().isostatic_step);
        } else if (name == "venus") {
            CHECK(offset < 1.0);
        }
    }
}

TEST_CASE("distance is a true distance near the ground", "[globe][generator]") {
    const Generator earth = generator_for("earth");
    constexpr double h = 0.5;
    for (std::uint64_t k = 0; k < 200; ++k) {
        const DVec3 down = direction(k);
        const double ground = hog::globe::ground_below(earth, down, 1.0).value_or(0.0);
        const DVec3 near = down * (ground + 3.0);
        CHECK(std::abs(earth.sample(down * ground, 1.0).distance) < 1e-3);
        DVec3 slope{0.0};
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            DVec3 step{0.0};
            step[axis] = h;
            slope[axis] = (earth.sample(near + step, 1.0).distance -
                           earth.sample(near - step, 1.0).distance) /
                          (2.0 * h);
        }
        CHECK(std::abs(glm::length(slope) - 1.0) < 0.05);
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
    CHECK(digest == 0xa78c'b253'3fc7'd89bULL);
}
