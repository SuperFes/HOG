#include <hog/formation/sphere.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::formation::Sphere;
using hog::formation::sphere_size;

TEST_CASE("a sphere has the points its splits make, all on the unit sphere", "[formation][sphere]") {
    const int subdivisions = GENERATE(0, 1, 4);
    const Sphere sphere{subdivisions, 7};
    REQUIRE(sphere.size() == sphere_size(subdivisions));
    CHECK(sphere.triangles().size() == 20U << (2U * static_cast<unsigned>(subdivisions)));
    for (std::uint32_t index = 0; index < sphere.size(); ++index) {
        CHECK_THAT(glm::length(sphere.point(index)), WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("every point has five or six neighbours, joined both ways", "[formation][sphere]") {
    const Sphere sphere{3, 11};
    int fives = 0;
    for (std::uint32_t index = 0; index < sphere.size(); ++index) {
        const auto around = sphere.neighbors(index);
        REQUIRE((around.size() == 5 || around.size() == 6));
        fives += around.size() == 5 ? 1 : 0;
        for (const std::uint32_t other : around) {
            const auto back = sphere.neighbors(other);
            CHECK(std::ranges::find(back, index) != back.end());
        }
    }
    CHECK(fives == 12);
}

TEST_CASE("areas cover the sphere, alike to within the split's unevenness", "[formation][sphere]") {
    const Sphere sphere{5, 3};
    double total = 0.0;
    double least = 1.0;
    double most = 0.0;
    for (std::uint32_t index = 0; index < sphere.size(); ++index) {
        total += sphere.area(index);
        least = std::min(least, sphere.area(index));
        most = std::max(most, sphere.area(index));
    }
    CHECK_THAT(total, WithinRel(4.0 * std::numbers::pi, 1e-12));
    CHECK(most / least < 2.5);
}

TEST_CASE("triangles wind anticlockwise seen from outside", "[formation][sphere]") {
    const Sphere sphere{4, 5};
    for (const auto& [a, b, c] : sphere.triangles()) {
        const DVec3& pa = sphere.point(a);
        CHECK(glm::dot(glm::cross(sphere.point(b) - pa, sphere.point(c) - pa), pa) > 0.0);
    }
}

TEST_CASE("the nearest point walked to is the nearest of all", "[formation][sphere]") {
    const Sphere sphere{4, 9};
    std::uint32_t start = 0;
    for (int n = 0; n < 500; ++n) {
        const double z = std::fmod((n * 0.618034) + 0.1, 2.0) - 1.0;
        const double turn = n * 2.399963;
        const double across = std::sqrt(1.0 - (z * z));
        const DVec3 direction{across * std::cos(turn), across * std::sin(turn), z};
        std::uint32_t best = 0;
        for (std::uint32_t index = 1; index < sphere.size(); ++index) {
            if (glm::dot(sphere.point(index), direction) > glm::dot(sphere.point(best), direction)) {
                best = index;
            }
        }
        start = sphere.nearest(direction, start);
        CHECK(start == best);
    }
}

TEST_CASE("a field read between points follows it smoothly", "[formation][sphere]") {
    const Sphere sphere{5, 2};
    std::vector<double> heights(sphere.size());
    for (std::uint32_t index = 0; index < sphere.size(); ++index) {
        heights[index] = sphere.point(index).x + (2.0 * sphere.point(index).z);
    }
    std::uint32_t start = 0;
    for (int n = 0; n < 300; ++n) {
        const double z = std::fmod((n * 0.381966) + 0.05, 2.0) - 1.0;
        const double turn = n * 2.399963;
        const double across = std::sqrt(1.0 - (z * z));
        const DVec3 direction{across * std::cos(turn), across * std::sin(turn), z};
        const Sphere::Spot spot = sphere.locate(direction, start);
        start = spot.corners[0];
        CHECK_THAT(spot.weights.x + spot.weights.y + spot.weights.z, WithinAbs(1.0, 1e-12));
        CHECK(glm::all(glm::greaterThanEqual(spot.weights, DVec3{0.0})));
        const double spacing = sphere.spacing();
        CHECK_THAT(sphere.sample(heights, spot),
                   WithinAbs(direction.x + (2.0 * direction.z), 3.0 * spacing * spacing));
    }
}

TEST_CASE("the same seed makes the same sphere, another seed another", "[formation][sphere]") {
    const Sphere one{3, 42};
    const Sphere same{3, 42};
    const Sphere other{3, 43};
    bool differs = false;
    for (std::uint32_t index = 0; index < one.size(); ++index) {
        CHECK(one.point(index) == same.point(index));
        differs = differs || one.point(index) != other.point(index);
    }
    CHECK(differs);
}

TEST_CASE("spacing halves with each split", "[formation][sphere]") {
    const Sphere coarse{4, 1};
    const Sphere fine{5, 1};
    CHECK_THAT(coarse.spacing() / fine.spacing(), WithinRel(2.0, 0.02));
}
