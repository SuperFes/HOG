#include "../src/formed.hpp"
#include "../src/joints.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::globe::Formed;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Planet;
using hog::globe::properties;

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

// What was carried onto the ground and settled where it lies.
double settled_on(const Formed::Here& here) {
    return here.layers.sand + here.layers.alluvium + here.layers.lake_beds + here.layers.till;
}

// How what was carried settled over places read beside each other.
struct Settling {
    // Read finely, and too coarsely for any hollow, as the points have it.
    struct Read {
        Formed::Here here;
        Formed::Here uncut;
        // How far the bed under what settled stands over, and the top over, the ground uncut.
        [[nodiscard]] double bed() const { return -here.cut; }
        [[nodiscard]] double top() const { return here.layers.beach + settled_on(here) - here.cut; }
        [[nodiscard]] bool covered() const { return settled_on(here) > 0.0; }
        [[nodiscard]] bool plain() const {
            return here.crag == 0.0 && here.layers.beach == 0.0 && uncut.layers.beach == 0.0 &&
                   uncut.layers.ice == 0.0;
        }
    };
    int edges = 0;
    int lower = 0;
    int deep = 0;
    int level = 0;
    int offshore = 0;
    int draped = 0;
    double carried = 0.0;
    double settled = 0.0;

    // Where it lies thin, it lies in the hollows, the crests between them bare.
    void across_edge(const Read& here, const Read& apart) {
        if (!apart.plain() || here.covered() == apart.covered()) {
            return;
        }
        ++edges;
        const Read& covered = here.covered() ? here : apart;
        const Read& bare = here.covered() ? apart : here;
        lower += covered.bed() < bare.bed() ? 1 : 0;
    }
    // Where it lies deep, its top runs smoother than the bed under it.
    void deep_over(const Read& here, const Read& next) {
        const double bed = std::abs(here.bed() - next.bed());
        if (!next.plain() || settled_on(here.here) <= 1.0 || settled_on(next.here) <= 1.0 ||
            bed <= 0.1) {
            return;
        }
        ++deep;
        level += std::abs(here.top() - next.top()) < bed ? 1 : 0;
    }
};

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
        CHECK_THAT(column.ground(), WithinAbs(planet.body.radius + here.top.value, 1e-6));
        if (here.top.value < -100.0) {
            CHECK(column.water() >= planet.body.radius);
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
                const double metres = (far - near) * generator.planet().body.radius;
                CHECK(std::abs(along(far) - along(near)) <
                      std::abs(under(far) - under(near)) + 1.0 + metres);
            }
        }
    }
    REQUIRE(walked > 0);
    CHECK(wet * 2 > walked);
}

namespace {

// The land points whose rivers carry the most water on to land or the sea, most first.
std::vector<std::uint32_t> largest_rivers(const hog::formation::World& world, std::size_t count) {
    std::vector<std::uint32_t> rivers;
    for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
        if (world.rivers.receiver[point] != point &&
            world.lakes.lake[point] == hog::formation::Lakes::none &&
            world.lakes.lake[world.rivers.receiver[point]] == hog::formation::Lakes::none) {
            rivers.push_back(point);
        }
    }
    const auto more = [&](std::uint32_t a, std::uint32_t b) {
        return world.lakes.discharge[a] > world.lakes.discharge[b];
    };
    const auto kept = std::min(count, rivers.size());
    std::ranges::partial_sort(rivers, rivers.begin() + static_cast<std::ptrdiff_t>(kept), more);
    rivers.resize(kept);
    return rivers;
}

} // namespace

namespace {

// Metres of a river's water over the ground.
double river_depth(const Formed::Here& here) {
    return here.river ? std::max(0.0, here.river->level - here.top.value) : 0.0;
}

// Narrowing in from between `low` and `high` on where the water read by `at` changes depth most,
// where a wall keeps its step however close: the two ends then.
template <class At>
std::pair<double, double> sharpest(const At& at, double low, double high) {
    for (int halving = 0; halving < 10; ++halving) {
        const double mid = 0.5 * (low + high);
        if (std::abs(river_depth(at(mid)) - river_depth(at(low))) >=
            std::abs(river_depth(at(high)) - river_depth(at(mid)))) {
            high = mid;
        } else {
            low = mid;
        }
    }
    return {low, high};
}

} // namespace

TEST_CASE("a formed river's water stands in its channel and meets its banks without walls",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double lod = generator.planet().voxel;
    const double radius = generator.planet().body.radius;
    int crossed = 0;
    int wet = 0;
    for (const std::uint32_t point : largest_rivers(world, 20)) {
        const DVec3 from = world.sphere.point(point);
        const DVec3 to = world.sphere.point(world.rivers.receiver[point]);
        const DVec3 middle = glm::normalize(from + to);
        const DVec3 across = glm::normalize(glm::cross(to - from, middle));
        const auto at = [&](double off) {
            return formed.at(glm::normalize(middle + (across * off)), lod);
        };
        if (at(0.0).top.value < 0.0) {
            continue;
        }
        ++crossed;
        // Across the valley as far as its course winds from the line, a metre at a time.
        const int half = static_cast<int>(0.35 * glm::length(to - from) * radius);
        bool any = false;
        Formed::Here last = at(-half / radius);
        for (int metres = -half; metres < half; ++metres) {
            const Formed::Here next = at((metres + 1) / radius);
            if (last.river) {
                any = true;
                // It stands over the ground, no higher than its banks in flood.
                CHECK(last.river->level > last.top.value - last.crag);
                CHECK(last.river->flood >= last.river->level);
            }
            if (river_depth(last) != river_depth(next)) {
                const auto [low, high] = sharpest(at, metres / radius, (metres + 1) / radius);
                const Formed::Here a = at(low);
                const Formed::Here b = at(high);
                CHECK(std::abs(river_depth(b) - river_depth(a)) <
                      std::abs(b.top.value - a.top.value) + 0.05 + ((high - low) * radius));
            }
            last = next;
        }
        wet += any ? 1 : 0;
    }
    REQUIRE(crossed > 10);
    CHECK(wet * 10 > crossed * 9);
}

TEST_CASE("a formed river's water stands level across its channel, no crag standing in it",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double lod = generator.planet().voxel;
    const double radius = generator.planet().body.radius;
    int steps = 0;
    int stepped = 0;
    for (const std::uint32_t point : largest_rivers(world, 20)) {
        const DVec3 from = world.sphere.point(point);
        const DVec3 to = world.sphere.point(world.rivers.receiver[point]);
        const DVec3 middle = glm::normalize(from + to);
        const DVec3 across = glm::normalize(glm::cross(to - from, middle));
        const auto at = [&](double off) {
            return formed.at(glm::normalize(middle + (across * off)), lod);
        };
        const int half = static_cast<int>(0.35 * glm::length(to - from) * radius);
        Formed::Here last = at(-half / radius);
        for (int metres = -half; metres < half; ++metres) {
            const Formed::Here next = at((metres + 1) / radius);
            // A metre apart in the same water, it stands all but as high, however the bed under
            // it rises and falls.
            if (last.river && next.river) {
                ++steps;
                stepped += std::abs(next.river->level - last.river->level) > 0.05 ? 1 : 0;
            }
            // The water plucks out any crag that would stand in it.
            if (last.river) {
                CHECK(last.crag == 0.0);
            }
            last = next;
        }
    }
    REQUIRE(steps > 100);
    CHECK(stepped * 50 < steps);
}

TEST_CASE("a formed river's water falls downstream", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double lod = generator.planet().voxel;
    const double radius = generator.planet().body.radius;
    int walked = 0;
    for (const std::uint32_t point : largest_rivers(world, 10)) {
        const DVec3 from = world.sphere.point(point);
        const DVec3 to = world.sphere.point(world.rivers.receiver[point]);
        const DVec3 across = glm::normalize(glm::cross(to - from, from));
        const int half = static_cast<int>(0.35 * glm::length(to - from) * radius);
        // Across each of a dozen stretches along it, 2 m at a time, the water nearest the line.
        std::vector<double> levels;
        constexpr int stretches = 12;
        for (int k = 1; k < stretches; ++k) {
            const DVec3 along = glm::normalize(from + ((to - from) * (k / double{stretches})));
            std::optional<double> level;
            int nearest = half;
            for (int metres = -half; metres < half; metres += 2) {
                const Formed::Here here =
                    formed.at(glm::normalize(along + (across * (metres / radius))), lod);
                if (here.river && std::abs(metres) < nearest) {
                    nearest = std::abs(metres);
                    level = here.river->level;
                }
            }
            if (level) {
                levels.push_back(*level);
            }
        }
        if (levels.size() < stretches / 2) {
            continue;
        }
        ++walked;
        // Its course winds past a stretch more than once, the nearest water a bend on.
        for (std::size_t k = 1; k < levels.size(); ++k) {
            CHECK(levels[k] <= levels[k - 1] + 1.0);
        }
    }
    CHECK(walked > 5);
}

TEST_CASE("a formed river's channel is left out with its water where read coarser than it's wide",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
    const double lod = generator.planet().voxel;
    const double radius = generator.planet().body.radius;
    int wet = 0;
    for (const std::uint32_t point : largest_rivers(world, 10)) {
        const DVec3 from = world.sphere.point(point);
        const DVec3 to = world.sphere.point(world.rivers.receiver[point]);
        const DVec3 middle = glm::normalize(from + to);
        const DVec3 across = glm::normalize(glm::cross(to - from, middle));
        const int half = static_cast<int>(0.35 * glm::length(to - from) * radius);
        for (int metres = -half; metres < half; ++metres) {
            const DVec3 direction = glm::normalize(middle + (across * (metres / radius)));
            if (formed.at(direction, lod).river) {
                ++wet;
                CHECK_FALSE(formed.at(direction, 1'000.0).river);
                break;
            }
        }
    }
    CHECK(wet > 5);
}

TEST_CASE("a formed lake that brims runs out over its rim at its level", "[globe][formed]") {
    const Formed& formed = *formed_generator().shape().formed;
    const auto outlets = formed.lake_outlets();
    WARN(outlets.size() << " lakes brim over their rims");
    for (const Formed::Outlet& outlet : outlets) {
        CHECK(std::abs(glm::length(outlet.at) - 1.0) < 1e-9);
        // The rim as the lake read it, before crags.
        const Formed::Here here = formed.at(outlet.at, 0.0);
        const double rim = here.top.value - here.crag;
        CAPTURE(rim, outlet.level);
        CHECK(std::abs(rim - outlet.level) < 0.5);
    }
}

TEST_CASE("a formed lake stands level, no higher than formed, over lower ground only",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const hog::formation::World& world = formed.world();
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
        CHECK(*here.lake <= world.lakes.lakes[lake].level + 1e-9);
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
    const double radius = generator.planet().body.radius;
    // Read coarser than the widest block of rock, whose crags step from the next by design.
    const double lod = hog::globe::widest_joints;
    for (const double pole : {1.0, -1.0}) {
        for (int ring = 0; ring < 40; ++ring) {
            const double from_pole = 0.002 * (ring + 1);
            const auto top = [&](double turn) {
                const DVec3 direction{std::sin(from_pole) * std::cos(turn),
                                      std::sin(from_pole) * std::sin(turn),
                                      pole * std::cos(from_pole)};
                return formed.at(direction, lod).top.value;
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
    CHECK(decoded->formed_detail == planet.formed_detail);
    Planet finer = planet;
    finer.formed_detail = 6.0;
    CHECK(world_id(finer) != world_id(planet));
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

TEST_CASE("formed rock stands out as crags only where it stands taller than its cover is deep",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    int land = 0;
    int crags = 0;
    double tallest = 0.0;
    for (std::uint64_t n = 0; n < 4'000; ++n) {
        const DVec3 direction = somewhere(n);
        const Formed::Here here = formed.at(direction, voxel);
        const Formed::Here::Weathering& rock = here.weathering;
        land += here.top.value > 0.0 ? 1 : 0;
        CHECK(rock.bare >= 0.0);
        CHECK(rock.bare <= 1.0);
        CHECK(rock.loosening <= rock.worn);
        // Standing no higher than the rock about it rotted and was stripped.
        const hog::formation::Body& body = formed.world().body;
        CHECK(here.crag <= hog::globe::rotted_by(rock.rate, std::max(rock.worn, body.slowest_wear),
                                                 body.soil_falloff, rock.years) +
                               1e-9);
        tallest = std::max(tallest, here.crag);
        if (here.crag <= 0.0) {
            continue;
        }
        // Joints grooved finer than a voxel don't stand out as crags.
        crags += here.crag >= voxel ? 1 : 0;
        // Bare rock: nothing laid, rotted or grown on it.
        CHECK(here.layers.ice == 0.0);
        CHECK(here.layers.loess + here.layers.sand + here.layers.alluvium + here.layers.lake_beds +
                  here.layers.till + here.layers.lava + here.layers.saprolite ==
              0.0);
        CHECK(here.layers.organic + here.layers.topsoil + here.layers.subsoil == 0.0);
        // Read too coarsely for its block, it's gone.
        CHECK(formed.at(direction, 1e9).crag == 0.0);
    }
    WARN("crags at " << crags << " of " << land << " on land, the tallest " << tallest << " m");
    CHECK(crags > 5);
    // Only the widest blocks' cores stand out, and only where rock rotted and was stripped.
    CHECK(crags < land / 10);
}

TEST_CASE("gullies cut down through what lies draped on formed ground, what was carried settling "
          "into them",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    const auto draped = [](const Formed::Here& here) {
        return here.layers.loess + here.layers.lava;
    };
    int cut_into = 0;
    for (std::uint64_t n = 0; n < 4'000; ++n) {
        const DVec3 direction = somewhere(n);
        // Read too coarsely for any gully, then finely.
        const Formed::Here uncut = formed.at(direction, 1e9);
        const Formed::Here here = formed.at(direction, voxel);
        // Crags come out of what's laid rather than being cut down through it.
        if (uncut.layers.ice > 0.0 || here.layers.ice > 0.0 || here.crag != 0.0) {
            continue;
        }
        const double cut = std::max(0.0, here.cut);
        CHECK_THAT(draped(here), WithinAbs(std::max(0.0, draped(uncut) - cut), 1e-6));
        // Less soil and beach, which form again on what's left.
        CHECK(here.layers.cover <=
              std::max(0.0, uncut.layers.cover - cut) + 1e-6 +
                  (here.layers.organic + here.layers.topsoil + here.layers.subsoil +
                   here.layers.beach + settled_on(here)));
        cut_into += cut > 0.0 && draped(uncut) > 0.0 ? 1 : 0;
    }
    CHECK(cut_into > 100);
}

TEST_CASE("what was carried onto formed ground lies in its hollows, filling them level where deep, "
          "draping the deep sea's bed",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    const double radius = generator.planet().body.radius;
    const auto read = [&](DVec3 direction) {
        return Settling::Read{.here = formed.at(direction, voxel),
                              .uncut = formed.at(direction, 1e9)};
    };
    Settling settling;
    for (std::uint64_t n = 0; n < 200'000; ++n) {
        const DVec3 direction = somewhere(n);
        const Formed::Here uncut = formed.at(direction, 1e9);
        if (uncut.layers.ice > 0.0 || settled_on(uncut) <= 0.0) {
            continue;
        }
        // Far under the sea, below where storms stir the bed, it drapes the bed as it falls.
        if (uncut.top.value < -500.0) {
            ++settling.offshore;
            settling.draped += settled_on(formed.at(direction, voxel)) > 0.0 ? 1 : 0;
            continue;
        }
        const Settling::Read here = read(direction);
        if (uncut.top.value < 0.0 || !here.plain()) {
            continue;
        }
        settling.settled += settled_on(here.here);
        settling.carried += settled_on(uncut);
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const auto beside = [&](double metres) {
            return read(glm::normalize(direction + (east * (metres / radius))));
        };
        settling.across_edge(here, beside(20.0));
        settling.deep_over(here, beside(6.0 * voxel));
    }
    UNSCOPED_INFO(settling.edges << " edges, the bed lower under it at " << settling.lower << "; "
                                 << settling.deep << " deep, its top smoother at " << settling.level
                                 << "; settled " << settling.settled << " of " << settling.carried
                                 << "; draped " << settling.draped << " of " << settling.offshore
                                 << " offshore");
    CHECK(settling.offshore > 20);
    CHECK(settling.draped > 9 * settling.offshore / 10);
    CHECK(settling.edges > 20);
    // The level it fills to changes a little across the gap, now and then past the bed.
    CHECK(settling.lower * 100 > 85 * settling.edges);
    CHECK(settling.deep > 20);
    CHECK(settling.level > 9 * settling.deep / 10);
    // As much over the ground as was carried to it, the bed spread about as evenly as not.
    CHECK(std::abs(settling.settled - settling.carried) < 0.25 * settling.carried);
}

TEST_CASE("beaches lie along formed shores as flat as the waves leave them, bare of soil",
          "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const double voxel = generator.planet().voxel;
    const double planet_radius = generator.planet().body.radius;
    // Storms' waves run up no higher than about twice the tallest fully grown sea's, near 10 m,
    // even on shingle.
    constexpr double tallest = 10.0;
    const double steepest = std::tan(properties(Material::Shingle).saturated_repose);
    int beaches = 0;
    int steep = 0;
    std::vector<double> sand;
    for (std::uint64_t n = 0; n < 200'000; ++n) {
        const DVec3 direction = somewhere(n);
        const Formed::Here here = formed.at(direction, voxel);
        if (here.layers.beach <= 0.0) {
            continue;
        }
        ++beaches;
        CHECK(here.top.value < 2.0 * tallest);
        CHECK(here.layers.organic + here.layers.topsoil + here.layers.subsoil == 0.0);
        const Material made_of = here.layers.beach_of;
        CHECK((made_of == Material::Sand || made_of == Material::VolcanicSand ||
               made_of == Material::ShellSand || made_of == Material::Shingle ||
               made_of == Material::Sediment));
        const double slope =
            glm::length(here.top.gradient - (glm::dot(here.top.gradient, direction) * direction));
        // How steep the ground under it is, across a metre either way.
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const DVec3 north = glm::cross(direction, east);
        const auto ground = [&](const DVec3& way) {
            const Formed::Here there =
                formed.at(glm::normalize(direction + (way / planet_radius)), voxel);
            return there.top.value - there.layers.beach;
        };
        const double under = std::hypot((ground(east) - ground(-east)) / 2.0,
                                        (ground(north) - ground(-north)) / 2.0);
        // On the shore, where the waves work it: further down it's what's slid off the shore.
        const bool shore = here.top.value > -2.0 * tallest;
        steep += shore && slope > steepest && under < steepest ? 1 : 0;
        // Over the shoreface, from the water down as far as storms move it; past that its toe
        // lies as steep as it holds.
        if (made_of == Material::Sand && here.top.value < 0.0 && here.top.value > -2.0 * tallest) {
            sand.push_back(slope);
        }
    }
    CHECK(beaches > 20);
    // No beach on the shore lies steeper than its stones hold soaked on ground that isn't already
    // as steep.
    CHECK(steep * 100 <= beaches);
    // Sand lies far flatter than it could where the waves work it.
    REQUIRE(sand.size() > 20);
    const auto middle = sand.begin() + static_cast<std::ptrdiff_t>(sand.size() / 2);
    std::ranges::nth_element(sand, middle);
    CHECK(*middle < 0.25 * std::tan(properties(Material::Sand).saturated_repose));
}

TEST_CASE("formed ground cost", "[.bench][globe][formed]") {
    // The working world's shell.
    Planet planet;
    planet.body.seed = 1;
    planet.body.radius = 30'000.0;
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

namespace {

// What a few hundred transects across a world's shores find.
struct Survey {
    double spacing = 0.0;
    int transects = 0;
    int with_beach = 0;
    int with_dry = 0;
    std::vector<double> waterline;   // spacings in from the sea point
    std::vector<double> slope_there; // the ground's at the waterline, beach or not
    std::vector<double> dry_width;
    std::array<std::vector<double>, 3> slopes; // over the water, to 20 m down, deeper
    std::array<int, hog::globe::material_count> kinds{};
};

// Metres from `from` towards `way` the ground comes out of the water, none within `reach`.
std::optional<double> waterline_from(const Formed& formed, const DVec3& from, const DVec3& way,
                                     double radius, double reach) {
    constexpr int steps = 1'200;
    for (int at = 0; at < steps; ++at) {
        const double metres = reach * at / steps;
        if (formed.at(glm::normalize(from + (way * (metres / radius))), 1e9).top.value > 0.0) {
            return metres;
        }
    }
    return std::nullopt;
}

// Across the shore at a waterline `crossing` metres from `from` towards `way`, half a spacing
// either side, every 2 m.
void across_waterline(const Formed& formed, const Planet& planet, const DVec3& from,
                      const DVec3& way, double crossing, Survey& survey) {
    constexpr double step = 2.0;
    const auto steps = static_cast<int>(survey.spacing / step);
    bool beach = false;
    double dry = 0.0;
    for (int at = 0; at < steps; ++at) {
        const double metres = crossing - (0.5 * survey.spacing) + (at * step);
        const DVec3 direction = glm::normalize(from + (way * (metres / planet.body.radius)));
        const Formed::Here here = formed.at(direction, planet.voxel);
        const double slope =
            glm::length(here.top.gradient - (glm::dot(here.top.gradient, direction) * direction));
        if (at == steps / 2) {
            survey.slope_there.push_back(slope);
        }
        if (here.layers.beach <= 0.0) {
            continue;
        }
        beach = true;
        ++survey.kinds[static_cast<std::size_t>(here.layers.beach_of)];
        const double top = here.top.value;
        std::size_t band = 2;
        if (top > 0.0) {
            band = 0;
            dry += step;
        } else if (top > -20.0) {
            band = 1;
        }
        survey.slopes[band].push_back(slope);
    }
    survey.with_beach += beach ? 1 : 0;
    survey.with_dry += dry > 0.0 ? 1 : 0;
    if (dry > 0.0) {
        survey.dry_width.push_back(dry);
    }
}

// From sea points beside land towards the land, to where the ground comes out of the water.
Survey survey_shores(const Planet& planet) {
    const Generator generator{planet};
    const Formed& formed = *generator.shape().formed;
    const auto& world = formed.world();
    const auto& sphere = world.sphere;
    Survey survey;
    survey.spacing = sphere.spacing() * planet.body.radius;
    const auto in_sea = [&](std::uint32_t point) { return world.rivers.receiver[point] == point; };
    for (std::uint32_t point = 0; point < sphere.size() && survey.transects < 300; point += 7) {
        const auto neighbors = sphere.neighbors(point);
        const auto land =
            std::ranges::find_if(neighbors, [&](std::uint32_t near) { return !in_sea(near); });
        if (!in_sea(point) || land == neighbors.end()) {
            continue;
        }
        const DVec3 from = sphere.point(point);
        const DVec3 way = glm::normalize(sphere.point(*land) - from);
        const std::optional<double> crossing =
            waterline_from(formed, from, way, planet.body.radius, 6.0 * survey.spacing);
        if (!crossing) {
            continue;
        }
        ++survey.transects;
        survey.waterline.push_back(*crossing / survey.spacing);
        across_waterline(formed, planet, from, way, *crossing, survey);
    }
    return survey;
}

// The tenth, middle and ninth tenth of `values`.
std::string spread(std::vector<double> values) {
    if (values.empty()) {
        return "none";
    }
    std::ostringstream text;
    for (const double share : {0.1, 0.5, 0.9}) {
        const auto at = values.begin() +
                        static_cast<std::ptrdiff_t>(share * static_cast<double>(values.size() - 1));
        std::ranges::nth_element(values, at);
        text << *at << (share < 0.9 ? " / " : "");
    }
    return text.str();
}

} // namespace

TEST_CASE("beaches across the shores of a few worlds", "[.bench][globe][formed]") {
    struct Shell {
        std::uint64_t seed = 1;
        double radius = 115'000.0;
        double detail = 5.0;
        double voxel = 2.5;
    };
    // The test planet's shell on a few seeds, and the development world's.
    for (const Shell shell : {Shell{.seed = 1}, Shell{.seed = 2}, Shell{.seed = 3},
                              Shell{.radius = 135'000.0, .detail = 7.0, .voxel = 1.0 / 3.0}}) {
        Planet planet;
        planet.body.seed = shell.seed;
        planet.body.radius = shell.radius;
        planet.formed_detail = shell.detail;
        planet.voxel = shell.voxel;
        const Survey survey = survey_shores(planet);
        const auto& kinds = survey.kinds;
        const auto kind = [&](Material material) {
            return kinds[static_cast<std::size_t>(material)];
        };
        WARN("seed " << shell.seed << " radius " << shell.radius << " detail " << shell.detail
                     << " (spacing " << survey.spacing << " m): transects " << survey.transects
                     << ", with beach " << survey.with_beach << ", with dry beach "
                     << survey.with_dry << "; waterline in from the sea point, spacings "
                     << spread(survey.waterline) << "; ground slope there "
                     << spread(survey.slope_there) << "; dry width " << spread(survey.dry_width)
                     << " m; beach slope over water " << spread(survey.slopes[0]) << ", to 20 m "
                     << spread(survey.slopes[1]) << ", deeper " << spread(survey.slopes[2])
                     << "; sand " << kind(Material::Sand) << ", mud " << kind(Material::Sediment)
                     << ", black " << kind(Material::VolcanicSand) << ", shell "
                     << kind(Material::ShellSand) << ", shingle " << kind(Material::Shingle));
    }
}

TEST_CASE("beaches meet the ground and the sea without walls or steps", "[globe][formed]") {
    const Generator& generator = formed_generator();
    const Formed& formed = *generator.shape().formed;
    const Planet& planet = generator.planet();
    const auto& world = formed.world();
    const auto& sphere = world.sphere;
    const double spacing = sphere.spacing() * planet.body.radius;
    const auto in_sea = [&](std::uint32_t point) { return world.rivers.receiver[point] == point; };
    int transects = 0;
    int with_beach = 0;
    for (std::uint32_t point = 0; point < sphere.size() && transects < 40; point += 13) {
        const auto neighbors = sphere.neighbors(point);
        const auto land =
            std::ranges::find_if(neighbors, [&](std::uint32_t near) { return !in_sea(near); });
        if (!in_sea(point) || land == neighbors.end()) {
            continue;
        }
        const DVec3 from = sphere.point(point);
        const DVec3 way = glm::normalize(sphere.point(*land) - from);
        const std::optional<double> crossing =
            waterline_from(formed, from, way, planet.body.radius, 6.0 * spacing);
        if (!crossing) {
            continue;
        }
        ++transects;
        // Every metre across, its thickness changes no faster than the ground under it does and
        // a slope as steep as loose ground holds, down to twice the tallest storms' waves.
        constexpr double step = 1.0;
        constexpr double shore_depth = 20.0;
        const auto steps = static_cast<int>(spacing / step);
        double last_beach = -1.0;
        double last_ground = 0.0;
        double last_top = 0.0;
        bool beach = false;
        for (int at = 0; at < steps; ++at) {
            const double metres = *crossing - (0.5 * spacing) + (at * step);
            const DVec3 direction = glm::normalize(from + (way * (metres / planet.body.radius)));
            const Formed::Here here = formed.at(direction, planet.voxel);
            const double ground = here.top.value - here.layers.beach;
            beach = beach || here.layers.beach > 0.0;
            // On the shore, where the waves work it: further down it's what's slid off.
            if (last_beach >= 0.0 && here.top.value > -shore_depth && last_top > -shore_depth) {
                CHECK(std::abs(here.layers.beach - last_beach) <=
                      std::abs(ground - last_ground) + (0.8 * step) + 0.05);
            }
            last_beach = here.layers.beach;
            last_ground = ground;
            last_top = here.top.value;
        }
        with_beach += beach ? 1 : 0;
    }
    CHECK(transects > 10);
    CHECK(with_beach > 3);
}

namespace {

// The ground of the working world about a place, read metres east and north of it.
struct Probe {
    Generator generator;
    DVec3 centre{0.0, 0.0, 1.0};
    DVec3 east{1.0, 0.0, 0.0};
    DVec3 north{0.0, 1.0, 0.0};
    double lod = 0.0;

    [[nodiscard]] Formed::Here at(double x, double y) const {
        const double radius = generator.planet().body.radius;
        return generator.shape().formed->at(
            glm::normalize(centre + (((east * x) + (north * y)) / radius)), lod);
    }
};

// NOLINTBEGIN(concurrency-mt-unsafe): read before any thread starts
std::optional<double> from_environment(const char* name) {
    const char* given = std::getenv(name);
    return given != nullptr ? std::optional{std::stod(given)} : std::nullopt;
}

std::vector<double> numbers_from_environment(const char* name) {
    std::vector<double> numbers;
    if (const char* given = std::getenv(name)) {
        std::istringstream read{given};
        for (double number = 0.0; read >> number;) {
            numbers.push_back(number);
            read.ignore(1);
        }
    }
    return numbers;
}
// NOLINTEND(concurrency-mt-unsafe)

// A profile through (x, y) along (dx, dy), `spacing` metres a step.
void print_profile(const Probe& probe, std::span<const double> line, double spacing) {
    const double length = std::hypot(line[2], line[3]);
    for (int k = -24; k <= 24; ++k) {
        const double t = k * spacing;
        const Formed::Here here =
            probe.at(line[0] + (line[2] / length * t), line[1] + (line[3] / length * t));
        std::println("{:6.2f}: top {:7.2f} cut {:6.2f} crag {:5.2f} lake {:7.2f}", t,
                     here.top.value, here.cut, here.crag, here.lake.value_or(-1.0));
    }
}

// Every column within `reach` standing over all four about it by more than a metre.
void print_spikes(const Probe& probe, int reach) {
    constexpr double step = 0.5;
    for (int row = -reach; row <= reach; ++row) {
        for (int column = -reach; column <= reach; ++column) {
            const double x = column * step;
            const double y = row * step;
            const Formed::Here here = probe.at(x, y);
            double about = -1e30;
            for (const auto& [dx, dy] : {std::pair{1.0, 0.0}, std::pair{-1.0, 0.0},
                                         std::pair{0.0, 1.0}, std::pair{0.0, -1.0}}) {
                about = std::max(about, probe.at(x + dx, y + dy).top.value);
            }
            if (here.top.value - about > 1.0) {
                std::println("{:6.1f} {:6.1f}: top {:7.2f} about {:7.2f} crag {:5.2f} lake {:7.2f}",
                             x, y, here.top.value, about, here.crag, here.lake.value_or(-1.0));
            }
        }
    }
}

} // namespace

// Prints what stands out of the ground round $HOG_PROBE_AT ("lat,lon" in degrees) on the working
// world: every column standing well over those about it, or with $HOG_PROBE_ACROSS
// ("x,y,dx,dy" metres) a profile through one, $HOG_PROBE_STEP metres a step, read at
// $HOG_PROBE_LOD.
TEST_CASE("what stands out of the ground round a place", "[.probe_spikes]") {
    const std::vector<double> place = numbers_from_environment("HOG_PROBE_AT");
    REQUIRE(place.size() == 2);
    auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet.has_value());
    Probe probe{.generator = Generator{planet.value()}};
    probe.centre = hog::planet::direction_at(place[0] * std::numbers::pi / 180.0,
                                             place[1] * std::numbers::pi / 180.0);
    probe.east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, probe.centre));
    probe.north = glm::cross(probe.centre, probe.east);
    probe.lod = from_environment("HOG_PROBE_LOD").value_or(probe.generator.planet().voxel);
    if (const std::vector<double> line = numbers_from_environment("HOG_PROBE_ACROSS");
        line.size() == 4) {
        print_profile(probe, line, from_environment("HOG_PROBE_STEP").value_or(0.25));
    } else {
        print_spikes(probe, 120);
    }
}
