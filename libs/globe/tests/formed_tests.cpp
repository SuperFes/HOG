#include "../src/formed.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::globe::Formed;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Planet;

namespace {

double unit(std::uint64_t bits) {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

DVec3 somewhere(std::uint64_t n) {
    const double z = (2.0 * unit(hog::hash_values(5, n, 0))) - 1.0;
    const double turn = 2.0 * std::numbers::pi * unit(hog::hash_values(5, n, 1));
    const double across = std::sqrt(1.0 - (z * z));
    return {across * std::cos(turn), across * std::sin(turn), z};
}

Planet formed_planet() {
    Planet planet;
    planet.formed_detail = 5;
    return planet;
}

const Generator& formed_generator() {
    static const Generator generator{formed_planet()};
    return generator;
}

} // namespace

TEST_CASE("a formed planet stands on the land that formed it, its sea at the radius",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    REQUIRE(generator.shape().formed);
    const Formed& formed = *generator.shape().formed;
    const Planet& planet = generator.planet();
    CHECK(generator.shape().sea);
    int under_sea = 0;
    for (std::uint64_t n = 0; n < 300; ++n) {
        const DVec3 direction = somewhere(n);
        const Generator::Column column = generator.column(direction, planet.voxel);
        const Formed::Here here = formed.at(direction);
        CHECK_THAT(column.ground(), WithinAbs(planet.radius + here.top.value, 1e-6));
        if (here.top.value < -100.0) {
            CHECK(column.water() >= planet.radius);
            ++under_sea;
        }
    }
    CHECK(under_sea > 0);
}

TEST_CASE("under a formed planet's ground its layers lie as they were laid", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double lod = generator.planet().voxel;
    int soiled = 0;
    int iced = 0;
    for (std::uint64_t n = 0; n < 2'000; ++n) {
        const DVec3 direction = somewhere(n);
        const Formed::Here here = formed.at(direction);
        const Generator::Column column = generator.column(direction, lod);
        const double top = column.ground();
        const hog::globe::Strata& layers = here.layers;
        const auto at_depth = [&](double depth) {
            return column.inside(top - depth - (lod / 2.0));
        };
        if (layers.ice > 10.0) {
            ++iced;
            CHECK(at_depth(layers.ice / 2.0) == Material::Ice);
        } else if (layers.ice == 0.0 && layers.topsoil > 0.2) {
            ++soiled;
            CHECK(at_depth(layers.organic + (layers.topsoil / 2.0)) == Material::Soil);
        }
        const Material deep = at_depth(layers.cover + 10.0);
        CHECK((deep == Material::Granite || deep == Material::Basalt));
    }
    CHECK(soiled > 0);
    CHECK(iced > 0);
}

TEST_CASE("a formed lake's water meets its shore and never stands as a wall", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double lod = generator.planet().voxel;
    const auto depth = [&](DVec3 direction) {
        const Generator::Column column = generator.column(direction, lod);
        return std::max(0.0, column.water() - column.ground());
    };
    const double step = 0.05 * world.sphere.spacing();
    int walked = 0;
    int wet = 0;
    for (std::uint32_t point = 0; point < world.sphere.size() && walked < 40; ++point) {
        if (world.lakes.lake[point] == hog::formation::Lakes::none ||
            world.sediment.lake_depth[point] <= 0.0) {
            continue;
        }
        ++walked;
        const DVec3 middle = world.sphere.point(point);
        // A lake only a point or two across can be smoothed away with the hollow holding it.
        wet += formed.at(middle).lake.has_value() ? 1 : 0;
        // Out across the shore every way, the water thinning as it goes: however steep the shore,
        // narrowing in on the steepest stretch shrinks the change, where a wall would keep it.
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, middle));
        const DVec3 north = glm::cross(middle, east);
        for (const DVec3 way : {east, north, -east, -north}) {
            const auto along = [&](double far) {
                return depth(glm::normalize(middle + (way * far)));
            };
            for (int n = 0; n < 60; ++n) {
                double near = step * n;
                double far = step * (n + 1);
                for (int halving = 0; halving < 10; ++halving) {
                    const double half = 0.5 * (near + far);
                    if (std::abs(along(half) - along(near)) >= std::abs(along(far) - along(half))) {
                        far = half;
                    } else {
                        near = half;
                    }
                }
                const double metres = (far - near) * generator.planet().radius;
                CHECK(std::abs(along(far) - along(near)) < 1.0 + metres);
            }
        }
    }
    REQUIRE(walked > 0);
    CHECK(wet * 2 > walked);
}

TEST_CASE("how finely a planet's land is formed is one of its knobs", "[globe][formed]") {
    Planet planet = formed_planet();
    CHECK(validate(planet).has_value());
    const auto decoded = hog::globe::decode_planet(hog::globe::encode_planet(planet));
    REQUIRE(decoded.has_value());
    CHECK(decoded->formed_detail == 5.0);
    CHECK(world_id(planet) != world_id(Planet{}));
    planet.formed_detail = 5.5;
    CHECK_FALSE(validate(planet).has_value());
    planet.formed_detail = 10.0;
    CHECK_FALSE(validate(planet).has_value());
}
