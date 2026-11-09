#include "../src/formed.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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
        const Formed::Here here = formed.at(direction, planet.voxel);
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
    int sanded = 0;
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
        if (layers.ice == 0.0 && layers.sand > lod) {
            ++sanded;
            const double soil = layers.organic + layers.topsoil + layers.subsoil;
            CHECK(at_depth(soil + layers.loess + (layers.sand / 2.0)) == Material::Sand);
        }
        const Material deep = at_depth(layers.cover + 10.0);
        CHECK((deep == Material::Granite || deep == Material::Basalt));
    }
    CHECK(soiled > 0);
    CHECK(iced > 0);
    CHECK(sanded > 0);
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
    const auto ground = [&](DVec3 direction) { return generator.column(direction, lod).ground(); };
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
        // Out across the shore every way, the water deepening and thinning only as the ground
        // under it falls and rises: narrowing in on the steepest stretch, a wall of water would
        // keep its change in depth however little the ground changed.
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, middle));
        const DVec3 north = glm::cross(middle, east);
        for (const DVec3 way : {east, north, -east, -north}) {
            const auto along = [&](double far) {
                return depth(glm::normalize(middle + (way * far)));
            };
            const auto under = [&](double far) {
                return ground(glm::normalize(middle + (way * far)));
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
                CHECK(std::abs(along(far) - along(near)) <
                      std::abs(under(far) - under(near)) + 1.0 + metres);
            }
        }
    }
    REQUIRE(walked > 0);
    CHECK(wet * 2 > walked);
}

TEST_CASE("a formed lake stands level, no higher than formed, over lower ground only",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double vertical = generator.derived().vertical;
    int paired = 0;
    for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
        const std::uint32_t lake = world.lakes.lake[point];
        if (lake == hog::formation::Lakes::none || world.sediment.lake_depth[point] <= 0.0) {
            continue;
        }
        const Formed::Here here = formed.at(world.sphere.point(point));
        if (!here.lake) {
            continue;
        }
        CHECK(*here.lake > here.top.value);
        CHECK(*here.lake <= (vertical * world.lakes.lakes[lake].level) + 1e-9);
        for (const std::uint32_t beside : world.sphere.neighbors(point)) {
            if (world.lakes.lake[beside] != lake) {
                continue;
            }
            if (const auto there = formed.at(world.sphere.point(beside)).lake) {
                CHECK(*there == *here.lake);
                ++paired;
            }
        }
    }
    CHECK(paired > 0);
}

TEST_CASE("a formed planet's ground reads whole everywhere, round the poles too",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double radius = generator.planet().radius;
    for (const double pole : {1.0, -1.0}) {
        for (int ring = 0; ring < 40; ++ring) {
            const double from_pole = 0.002 * (ring + 1);
            const auto top = [&](double turn) {
                const DVec3 direction{std::sin(from_pole) * std::cos(turn),
                                      std::sin(from_pole) * std::sin(turn),
                                      pole * std::cos(from_pole)};
                return formed.at(direction).top.value;
            };
            constexpr int steps = 100;
            for (int n = 0; n < steps; ++n) {
                double near = 2.0 * std::numbers::pi * n / steps;
                double far = 2.0 * std::numbers::pi * (n + 1) / steps;
                REQUIRE(std::isfinite(top(near)));
                // However steep, narrowing in on the steepest stretch shrinks the change; a
                // break in the ground would keep it.
                for (int halving = 0; halving < 20; ++halving) {
                    const double half = 0.5 * (near + far);
                    if (std::abs(top(half) - top(near)) >= std::abs(top(far) - top(half))) {
                        far = half;
                    } else {
                        near = half;
                    }
                }
                const double metres = (far - near) * std::sin(from_pole) * radius;
                CHECK(std::abs(top(far) - top(near)) < 1.0 + (5.0 * metres));
            }
        }
    }
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

TEST_CASE("soil lies on formed ground only as steep as it holds", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    // Dry soil holds up to its repose, soaked soil less.
    const double steepest = std::tan(hog::globe::properties(Material::Soil).repose);
    int soiled = 0;
    int steep = 0;
    for (std::uint64_t n = 0; n < 4'000; ++n) {
        const DVec3 direction = somewhere(n);
        const Formed::Here here = formed.at(direction, voxel);
        const DVec3 gradient = here.top.gradient;
        const double slope = glm::length(gradient - (glm::dot(gradient, direction) * direction));
        const double soil = here.layers.organic + here.layers.topsoil + here.layers.subsoil;
        soiled += soil > 0.0 ? 1 : 0;
        if (slope >= steepest) {
            ++steep;
            CHECK(soil == 0.0);
        }
    }
    CHECK(soiled > 1'000);
    CHECK(steep > 10);
}

TEST_CASE("gullies cut down through what was laid on formed ground", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    const auto laid = [](const Formed::Here& here) {
        return here.layers.loess + here.layers.sand + here.layers.alluvium + here.layers.lake_beds +
               here.layers.till + here.layers.lava;
    };
    int cut_into = 0;
    for (std::uint64_t n = 0; n < 4'000; ++n) {
        const DVec3 direction = somewhere(n);
        // Read too coarsely for any gully, then finely.
        const Formed::Here uncut = formed.at(direction, 1e9);
        const Formed::Here here = formed.at(direction, voxel);
        if (uncut.layers.ice > 0.0 || here.layers.ice > 0.0) {
            continue;
        }
        const double cut = std::max(0.0, uncut.top.value - here.top.value);
        CHECK_THAT(laid(here), WithinAbs(std::max(0.0, laid(uncut) - cut), 1e-6));
        CHECK(here.layers.cover <=
              std::max(0.0, uncut.layers.cover - cut) + 1e-6 +
                  (here.layers.organic + here.layers.topsoil + here.layers.subsoil));
        cut_into += cut > 0.0 && laid(uncut) > 0.0 ? 1 : 0;
    }
    CHECK(cut_into > 100);
}

TEST_CASE("formed ground cost", "[.bench][globe][formed]") {
    // The working world's shell.
    Planet planet;
    planet.seed = 1;
    planet.radius = 30'000.0;
    planet.formed_detail = 7;
    const auto seconds = [](auto from) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
    };
    auto started = std::chrono::steady_clock::now();
    const Generator generator{planet};
    WARN("made in " << seconds(started) << " s");
    const Formed& formed = *generator.shape().formed;
    for (const double lod : {1e9, 40.0, 2.5}) {
        double sink = 0.0;
        started = std::chrono::steady_clock::now();
        constexpr int reads = 20'000;
        for (int n = 0; n < reads; ++n) {
            sink += formed.at(somewhere(static_cast<std::uint64_t>(n)), lod).top.value;
        }
        WARN("lod " << lod << ": " << seconds(started) / reads * 1e6 << " us a read (" << sink
                    << ")");
    }
}
