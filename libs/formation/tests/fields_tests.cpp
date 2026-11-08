#include <hog/formation/fields.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::formation::Nearest;
using hog::formation::nearest_sources;
using hog::formation::Source;
using hog::formation::Sphere;
using hog::formation::sphere_noise;

TEST_CASE("sphere noise stays in range, varies, and is the same for a seed", "[formation][fields]") {
    const Sphere sphere{5, 1};
    const std::vector<double> noise = sphere_noise(sphere, 99, 2, 5);
    const std::vector<double> again = sphere_noise(sphere, 99, 2, 5);
    REQUIRE(noise.size() == sphere.size());
    CHECK(noise == again);
    const auto [least, most] = std::ranges::minmax(noise);
    CHECK(least >= -1.0);
    CHECK(most <= 1.0);
    CHECK(most - least > 0.5);
}

TEST_CASE("sphere noise changes little between neighbours", "[formation][fields]") {
    const Sphere sphere{6, 1};
    const std::vector<double> noise = sphere_noise(sphere, 5, 2, 4);
    double steepest = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        for (const std::uint32_t other : sphere.neighbors(point)) {
            steepest = std::max(steepest, std::abs(noise[point] - noise[other]));
        }
    }
    // Each level can change by about 0.07 a step at these spacings; a seam would jump near 1.
    CHECK(steepest < 0.3);
}

TEST_CASE("the nearest source spread over the points is the truly nearest", "[formation][fields]") {
    const Sphere sphere{5, 4};
    std::vector<Source> sources;
    for (std::uint32_t point = 17; point < sphere.size(); point += 997) {
        const DVec3 off = glm::normalize(sphere.point(point) + DVec3{0.002, -0.001, 0.0015});
        sources.push_back({.where = off, .point = point});
    }
    const Nearest nearest = nearest_sources(sphere, sources);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        double best = 10.0;
        for (const Source& source : sources) {
            const DVec3& here = sphere.point(point);
            best = std::min(best, std::atan2(glm::length(glm::cross(here, source.where)),
                                             glm::dot(here, source.where)));
        }
        CHECK_THAT(nearest.distance[point], WithinAbs(best, 1e-9));
    }
}

TEST_CASE("with no sources nothing is near", "[formation][fields]") {
    const Sphere sphere{2, 4};
    const Nearest nearest = nearest_sources(sphere, {});
    CHECK(std::ranges::all_of(nearest.source, [](std::uint32_t s) { return s == Nearest::none; }));
}
