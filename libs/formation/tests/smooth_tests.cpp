#include <hog/core/hash.hpp>
#include <hog/formation/smooth.hpp>
#include <hog/formation/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::formation::Body;
using hog::formation::Smooth;
using hog::formation::Sphere;
using hog::formation::World;

namespace {

double unit(std::uint64_t bits) {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

DVec3 somewhere(std::uint64_t n) {
    const double z = (2.0 * unit(hog::hash_values(7, n, 0))) - 1.0;
    const double turn = 2.0 * std::numbers::pi * unit(hog::hash_values(7, n, 1));
    const double across = std::sqrt(1.0 - (z * z));
    return {across * std::cos(turn), across * std::sin(turn), z};
}

DVec3 tangent(const DVec3& at) {
    return glm::normalize(glm::cross(at, std::abs(at.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{1, 0, 0}));
}

} // namespace

TEST_CASE("a level field reads level everywhere", "[formation][smooth]") {
    const Sphere sphere{4, 3};
    const Smooth smooth{sphere};
    const std::vector<double> level(sphere.size(), 42.0);
    Smooth::Weights weights;
    for (std::uint64_t n = 0; n < 200; ++n) {
        smooth.weigh(somewhere(n), weights);
        const Smooth::Read read = Smooth::read(level, weights);
        CHECK_THAT(read.value, WithinAbs(42.0, 1e-9));
        CHECK(glm::length(read.gradient) < 1e-9);
    }
}

TEST_CASE("every point a bump reaches counts, and the counts sum to 1", "[formation][smooth]") {
    const Sphere sphere{4, 3};
    const Smooth smooth{sphere};
    Smooth::Weights weights;
    for (std::uint64_t n = 0; n < 200; ++n) {
        const DVec3 at = somewhere(n);
        smooth.weigh(at, weights);
        double sum = 0.0;
        for (const double weight : weights.weights) {
            CHECK(weight >= 0.0);
            sum += weight;
        }
        CHECK_THAT(sum, WithinAbs(1.0, 1e-12));
        for (std::uint32_t point = 0; point < sphere.size(); ++point) {
            const DVec3& p = sphere.point(point);
            const double away = std::atan2(glm::length(glm::cross(at, p)), glm::dot(at, p));
            if (away < smooth.reach()) {
                CHECK(std::ranges::find(weights.points, point) != weights.points.end());
            }
        }
    }
}

TEST_CASE("a field reads without creases: its slope is the change across it",
          "[formation][smooth]") {
    const Sphere sphere{4, 3};
    const Smooth smooth{sphere};
    std::vector<double> field(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        field[point] = 1'000.0 * unit(hog::hash_values(11, point));
    }
    Smooth::Weights weights;
    const double step = 1e-6;
    for (std::uint64_t n = 0; n < 200; ++n) {
        const DVec3 at = somewhere(n);
        const DVec3 across = tangent(at);
        smooth.weigh(at, weights);
        const Smooth::Read read = Smooth::read(field, weights);
        smooth.weigh(glm::normalize(at + (step * across)), weights);
        const double ahead = Smooth::read(field, weights).value;
        smooth.weigh(glm::normalize(at - (step * across)), weights);
        const double behind = Smooth::read(field, weights).value;
        const double change = (ahead - behind) / (2.0 * step);
        CHECK_THAT(glm::dot(read.gradient, across),
                   WithinAbs(change, 1e-3 * (1.0 + std::abs(change))));
    }
}

TEST_CASE("a world forms every stage over the same points", "[formation][smooth]") {
    const World world{Body{}, 4, 5};
    const std::size_t size = world.sphere.size();
    CHECK(world.rivers.height.size() == size);
    CHECK(world.soil.depth.size() == size);
    CHECK(world.outcrop.rock.size() == size);
    CHECK(world.ores.coal.size() == size);
    CHECK(world.landforms.mesas.size() == size);
}
