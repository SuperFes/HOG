#include "../src/courses.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using hog::DVec3;
using hog::globe::Corridor;
using hog::globe::course;

namespace {

// A kilometre-long corridor on a planet 100 km round, running east along the equator.
constexpr double radius = 100'000.0;
const Corridor corridor{.from = DVec3{1.0, 0.0, 0.0},
                        .to = glm::normalize(DVec3{1.0, 1'000.0 / radius, 0.0}),
                        .half_width = 300.0 / radius,
                        .steps = 24,
                        .lanes = 6};

// Metres north of the straight line.
double aside(DVec3 at) {
    return at.z * radius;
}

} // namespace

TEST_CASE("water cuts straight across rock that is the same everywhere", "[globe][courses]") {
    const std::vector<DVec3> line = course(corridor, [](DVec3) { return 1.0; });
    REQUIRE(line.size() > 25);
    CHECK(line.front() == corridor.from);
    CHECK(line.back() == corridor.to);
    for (const DVec3& at : line) {
        CHECK(std::abs(aside(at)) < 1e-6);
    }
}

TEST_CASE("water cuts along the weak rock beside its way", "[globe][courses]") {
    // A crushed band along a fault 150 m north, ten times as easily cut.
    const auto effort = [](DVec3 at) { return std::abs(aside(at) - 150.0) < 40.0 ? 0.1 : 1.0; };
    const std::vector<DVec3> line = course(corridor, effort);
    CHECK(line.front() == corridor.from);
    CHECK(line.back() == corridor.to);
    int in_band = 0;
    for (const DVec3& at : line) {
        CHECK(aside(at) > -1.0);
        CHECK(aside(at) < 300.0 + 1.0);
        in_band += std::abs(aside(at) - 150.0) < 40.0 ? 1 : 0;
    }
    CHECK(in_band * 2 > static_cast<int>(line.size()));
}

TEST_CASE("water's course bends no sharper than its corners are worn", "[globe][courses]") {
    // Alternating hard and soft ground, so the cheapest way zigzags.
    const auto effort = [](DVec3 at) {
        return std::sin(at.y * radius / 40.0) * aside(at) > 0.0 ? 0.2 : 1.0;
    };
    const std::vector<DVec3> line = course(corridor, effort);
    for (std::size_t k = 1; k + 1 < line.size(); ++k) {
        const DVec3 in = glm::normalize(line[k] - line[k - 1]);
        const DVec3 out = glm::normalize(line[k + 1] - line[k]);
        // A step aside is a lane over a step along: worn twice, no turn is near a right angle.
        CHECK(glm::dot(in, out) > 0.5);
    }
}
