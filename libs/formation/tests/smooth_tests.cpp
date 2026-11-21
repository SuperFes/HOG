#include <hog/core/hash.hpp>
#include <hog/formation/smooth.hpp>
#include <hog/formation/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
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

TEST_CASE("a field reads within its range anywhere in a cap", "[formation][smooth]") {
    const Sphere sphere{5, 1, 0.2};
    const Smooth smooth{sphere};
    std::vector<double> field(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        field[point] = unit(hog::hash_values(3, point));
    }
    Smooth::Weights weights;
    for (std::uint64_t n = 0; n < 50; ++n) {
        const DVec3 middle = somewhere(n);
        for (const double within : {0.0, 0.3 * sphere.spacing(), 3.0 * sphere.spacing()}) {
            const Smooth::Range range = smooth.range(field, middle, within);
            for (std::uint64_t k = 0; k < 200; ++k) {
                // Anywhere in the cap, out to its edge.
                const double off = within * std::sqrt(unit(hog::hash_values(5, n, k)));
                const DVec3 at = glm::normalize(
                    middle +
                    (glm::normalize(glm::cross(middle, somewhere(k + 1000))) * std::tan(off)));
                smooth.weigh(at, weights);
                const double value = Smooth::read(field, weights).value;
                CHECK(value >= range.least - 1e-12);
                CHECK(value <= range.most + 1e-12);
            }
        }
    }
}

namespace {

// The weights at `s`, `t` across a square `side` radians on a side about `middle`, read from its
// corners, and the place they're for.
struct Between {
    DVec3 at{0.0};
    Smooth::Weights weights;
};
Between weighed_between(const Smooth& smooth, const DVec3& middle, double side, double s,
                        double t) {
    const DVec3 u = tangent(middle) * side;
    const DVec3 v = glm::cross(middle, tangent(middle)) * side;
    const DVec3 corner = middle - ((u + v) * 0.5);
    std::array<Smooth::Weights, 4> known;
    std::array<Smooth::Corner, 4> corners;
    for (std::size_t c = 0; c < 4; ++c) {
        corners.at(c).direction = glm::normalize(corner + (u * static_cast<double>(c & 1U)) +
                                                 (v * static_cast<double>(c >> 1U)));
        smooth.weigh(corners.at(c).direction, known.at(c));
        corners.at(c).weights = &known.at(c);
    }
    Between between{.at = glm::normalize(corner + (u * s) + (v * t)), .weights = {}};
    Smooth::between(between.at, corners, s, t, between.weights);
    return between;
}

} // namespace

TEST_CASE("weights read between four places agree with those weighed there, closer the smaller "
          "the square",
          "[formation][smooth]") {
    const Sphere sphere{4, 3};
    const Smooth smooth{sphere};
    std::vector<double> field(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        field[point] = 1'000.0 * unit(hog::hash_values(13, point));
    }
    Smooth::Weights weighed;
    double coarser = 0.0;
    for (const double share : {0.5, 0.25, 0.125}) {
        double worst = 0.0;
        double worst_slope = 0.0;
        for (std::uint64_t n = 0; n < 200; ++n) {
            const double s = unit(hog::hash_values(17, n, 0));
            const double t = unit(hog::hash_values(17, n, 1));
            const Between between =
                weighed_between(smooth, somewhere(n), share * sphere.spacing(), s, t);
            double sum = 0.0;
            for (const double weight : between.weights.weights) {
                sum += weight;
            }
            CHECK_THAT(sum, WithinAbs(1.0, 1e-9));
            smooth.weigh(between.at, weighed);
            const Smooth::Read read = Smooth::read(field, between.weights);
            const Smooth::Read truth = Smooth::read(field, weighed);
            worst = std::max(worst, std::abs(read.value - truth.value));
            worst_slope = std::max(worst_slope, glm::length(read.gradient - truth.gradient) /
                                                    (1.0 + glm::length(truth.gradient)));
            CHECK(std::abs(glm::dot(read.gradient, between.at)) < 1e-9);
        }
        WARN("share " << share << " worst " << worst << " slope " << worst_slope);
        if (coarser > 0.0) {
            CHECK(worst < coarser / 4.0);
        }
        coarser = worst;
    }
    CHECK(coarser < 1.0);
}
