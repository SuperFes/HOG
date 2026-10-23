#include "probe.hpp"
#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/render/gusts.hpp>
#include <hog/shaders/gusts_probe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::WorldPosition;
using hog::render::gust_anchor;
using hog::render::gust_drift;
using hog::render::gust_period;
using hog::render::testing::probed;
using hog::render::testing::Probed;
using hog::render::testing::test_device;

namespace {

constexpr double radius = 115'000.0;

// A place on the ground, and east and north along it.
struct Ground {
    DVec3 up = glm::normalize(DVec3{0.3, -0.5, 0.8});
    DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    DVec3 north = glm::cross(up, east);
    WorldPosition here{up * radius};

    [[nodiscard]] WorldPosition at(double east_of, double north_of) const {
        return here + (east * east_of) + (north * north_of);
    }
};

struct Gusting {
    double gust = 0.0;  // how hard the gusts blow, 0 to 1
    double speed = 0.0; // m/s
    double sheen = 0.0; // how much further grass is laid over than on the whole
};

// How hard the gusts blow at each point, how fast the wind blows there and how it lays grass over,
// seen from `eye` with the field drifted `drifted`, the wind blowing `speed` on the whole.
std::vector<Gusting> gusting_at(hog::render::Device& device, const WorldPosition& eye,
                                DVec3 drifted, std::span<const WorldPosition> points,
                                double speed) {
    const glm::vec3 anchor{gust_anchor(eye, drifted)};
    std::vector<Probed> asked;
    for (const WorldPosition& point : points) {
        const glm::vec3 from_eye{point - eye};
        asked.push_back({from_eye.x, from_eye.y, from_eye.z, static_cast<float>(speed)});
        asked.push_back({anchor.x, anchor.y, anchor.z, 0.0F});
    }
    std::vector<Gusting> gusting;
    for (const Probed& found : probed(device, hog::shaders::gusts_probe, asked, points.size())) {
        gusting.push_back({.gust = static_cast<double>(found[0]),
                           .speed = static_cast<double>(found[1]),
                           .sheen = static_cast<double>(found[2])});
    }
    return gusting;
}

std::vector<double> gusts_at(hog::render::Device& device, const WorldPosition& eye, DVec3 drifted,
                             std::span<const WorldPosition> points) {
    std::vector<double> gusts;
    for (const Gusting& found : gusting_at(device, eye, drifted, points, 0.0)) {
        gusts.push_back(found.gust);
    }
    return gusts;
}

// A square of ground `side` metres across, a point every `step`, rows running east.
std::vector<WorldPosition> square(const Ground& ground, double side, double step) {
    const auto steps = static_cast<int>(side / step);
    std::vector<WorldPosition> points;
    for (int north = 0; north < steps; ++north) {
        for (int east = 0; east < steps; ++east) {
            points.push_back(ground.at(east * step, north * step));
        }
    }
    return points;
}

std::vector<WorldPosition> moved(std::vector<WorldPosition> points, DVec3 by) {
    for (WorldPosition& point : points) {
        point += by;
    }
    return points;
}

double mean_difference(std::span<const double> a, std::span<const double> b) {
    double sum = 0.0;
    for (std::size_t at = 0; at < a.size(); ++at) {
        sum += std::abs(a[at] - b[at]);
    }
    return sum / static_cast<double>(a.size());
}

} // namespace

TEST_CASE("gusts are carried downwind faster than the wind near the ground, turning as they go",
          "[render]") {
    const Ground ground;
    const DVec3 drift = gust_drift(ground.east * 4.0, ground.up);
    CHECK(glm::dot(drift, ground.east) > 4.0);
    CHECK_THAT(glm::dot(drift, ground.north), WithinAbs(0.0, 1e-9));
    CHECK(glm::dot(drift, ground.up) > 0.0);
    CHECK(gust_drift(DVec3{0.0}, ground.up) == DVec3{0.0});
}

TEST_CASE("the gusts' field lies within its period from any eye", "[render]") {
    const DVec3 drifted{-1'234.5, 98'765.4, 3.0};
    for (const WorldPosition& eye :
         {WorldPosition{0.0, 0.0, 0.0}, WorldPosition{-115'000.0, 20.5, 3'000.25},
          WorldPosition{40'000.0, -90'000.0, 64'000.0}}) {
        const DVec3 anchor = gust_anchor(eye, drifted);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            CHECK(anchor[axis] >= 0.0);
            CHECK(anchor[axis] < gust_period);
        }
        CHECK(glm::length(gust_anchor(eye + DVec3{gust_period, 0.0, -2.0 * gust_period}, drifted) -
                          anchor) < 1e-6);
        CHECK(glm::length(gust_anchor(eye, drifted + DVec3{0.0, 3.0 * gust_period, 0.0}) - anchor) <
              1e-6);
    }
}

TEST_CASE("gusts lie on the ground, the same seen from anywhere", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    const DVec3 drifted{17.0, -3.0, 250.0};
    const WorldPosition standing = ground.here + (ground.up * 1.7);
    const WorldPosition flying =
        standing + (ground.east * 300.0) - (ground.north * 150.0) + (ground.up * 40.0);
    const std::vector<double> seen = gusts_at(*device, standing, drifted, points);
    const std::vector<double> from_afar = gusts_at(*device, flying, drifted, points);
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(from_afar[at], WithinAbs(seen[at], 0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gusts drift downwind", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    const DVec3 drifted{17.0, -3.0, 250.0};
    const DVec3 blown = ground.east * 60.0;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const std::vector<double> before = gusts_at(*device, eye, drifted, points);
    const std::vector<double> after = gusts_at(*device, eye, drifted + blown, moved(points, blown));
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(after[at], WithinAbs(before[at], 0.01));
    }
    // Rather than standing still.
    CHECK(mean_difference(before, gusts_at(*device, eye, drifted + blown, points)) > 0.1);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the gusts' field shows no seam where its period wraps", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    // Drifted so the field lies just short of its period from the eye, then just past it.
    const DVec3 short_of = eye.metres() - DVec3{gust_period - 50.0};
    const DVec3 past = short_of + DVec3{100.0};
    const std::vector<double> before = gusts_at(*device, eye, short_of, points);
    const std::vector<double> after = gusts_at(*device, eye, past, moved(points, DVec3{100.0}));
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(after[at], WithinAbs(before[at], 0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gusts blow in patches tens of metres across, with lulls between", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const DVec3 drifted{17.0, -3.0, 250.0};
    const std::vector<WorldPosition> points = square(ground, 600.0, 3.0);
    const auto shifted = [&](double east) {
        return gusts_at(*device, eye, drifted, moved(points, ground.east * east));
    };
    const std::vector<double> field = shifted(0.0);
    const auto share = [&](auto&& kept) {
        return static_cast<double>(std::ranges::count_if(field, kept)) /
               static_cast<double>(field.size());
    };
    const double gusting = share([](double gust) { return gust > 0.75; });
    const double lull = share([](double gust) { return gust < 0.25; });
    const double metre = mean_difference(field, shifted(1.0));
    const double near = mean_difference(field, shifted(5.0));
    const double apart = mean_difference(field, shifted(60.0));
    INFO("gusting " << gusting << ", lull " << lull << "; differing 1 m apart " << metre << ", 5 m "
                    << near << ", 60 m " << apart);
    CHECK(gusting > 0.15);
    CHECK(gusting < 0.45);
    CHECK(lull > 0.3);
    CHECK(lull < 0.7);
    CHECK(metre < 0.05);
    CHECK(near < 0.5 * apart);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the wind slackens in the lulls and blows harder in the gusts, as hard on the whole",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const std::vector<WorldPosition> points = square(ground, 600.0, 3.0);
    constexpr double speed = 6.0;
    double sum = 0.0;
    for (const Gusting& found : gusting_at(*device, eye, DVec3{0.0}, points, speed)) {
        if (found.gust < 0.1) {
            CHECK(found.speed < 0.7 * speed);
        }
        if (found.gust > 0.9) {
            CHECK(found.speed > 1.3 * speed);
        }
        sum += found.speed;
    }
    const double mean = sum / static_cast<double>(points.size());
    INFO("blowing " << mean << " m/s on the whole");
    CHECK(mean > 0.85 * speed);
    CHECK(mean < 1.15 * speed);
    for (const Gusting& found : gusting_at(*device, eye, DVec3{0.0}, points, 0.0)) {
        CHECK(found.speed == 0.0);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("grass is laid further over in the gusts and stands up in the lulls", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const std::vector<WorldPosition> points = square(ground, 600.0, 3.0);
    for (const Gusting& found : gusting_at(*device, eye, DVec3{0.0}, points, 6.0)) {
        if (found.gust < 0.1) {
            CHECK(found.sheen < -0.15);
        }
        if (found.gust > 0.9) {
            CHECK(found.sheen > 0.1);
        }
    }
    for (const Gusting& found : gusting_at(*device, eye, DVec3{0.0}, points, 0.0)) {
        CHECK(found.sheen == 0.0);
    }
    CHECK(device->validation_errors() == 0);
}
