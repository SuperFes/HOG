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
#include <numbers>
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
    double gust = 0.0;   // how hard the gusts blow, 0 to 1
    double speed = 0.0;  // m/s
    double laid = 0.0;   // how much further grass is laid over than on the whole
    double sheen = 0.0;  // and swaying with it
    double bush = 0.0;   // how far a bush's top is held over, of its height
    double tree = 0.0;   // and a tree's
    double thick = 0.0;  // times its density grass grows, the field lying where the gusts' does
    double taller = 0.0; // times its height grass stands for it
};

// How hard the gusts blow at each point, how fast the wind blows there and how it lays grass over,
// seen from `eye` with the field drifted `drifted`, the wind blowing `speed` on the whole and
// the grass swayed for `seconds`.
std::vector<Gusting> gusting_at(hog::render::Device& device, const WorldPosition& eye,
                                DVec3 drifted, std::span<const WorldPosition> points, double speed,
                                double seconds = 0.0) {
    const glm::vec3 anchor{gust_anchor(eye, drifted)};
    std::vector<Probed> asked;
    for (const WorldPosition& point : points) {
        const glm::vec3 from_eye{point - eye};
        asked.push_back({from_eye.x, from_eye.y, from_eye.z, static_cast<float>(speed)});
        asked.push_back({anchor.x, anchor.y, anchor.z, static_cast<float>(seconds)});
    }
    const std::vector<Probed> found =
        probed(device, hog::shaders::gusts_probe, asked, 2 * points.size());
    std::vector<Gusting> gusting;
    for (std::size_t at = 0; at < points.size(); ++at) {
        const Probed& grass = found[2 * at];
        const Probed& woody = found[(2 * at) + 1];
        gusting.push_back({.gust = static_cast<double>(grass[0]),
                           .speed = static_cast<double>(grass[1]),
                           .laid = static_cast<double>(grass[2]),
                           .sheen = static_cast<double>(grass[3]),
                           .bush = static_cast<double>(woody[0]),
                           .tree = static_cast<double>(woody[1]),
                           .thick = static_cast<double>(woody[2]),
                           .taller = static_cast<double>(woody[3])});
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
            CHECK(found.laid < -0.15);
        }
        if (found.gust > 0.9) {
            CHECK(found.laid > 0.1);
        }
    }
    for (const Gusting& found : gusting_at(*device, eye, DVec3{0.0}, points, 0.0, 3.0)) {
        CHECK(found.laid == 0.0);
        CHECK(found.sheen == 0.0);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("grass sways about how far the gusts lay it over, in waves across the field",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const std::vector<WorldPosition> points = square(ground, 120.0, 4.0);
    constexpr double speed = 6.0;
    // A whole sway and more, in even steps through it.
    constexpr double turning = 1.4 + (0.3 * speed);
    constexpr int steps = 16;
    std::vector<double> mean(points.size(), 0.0);
    std::vector<double> least(points.size(), 1.0);
    std::vector<double> most(points.size(), -1.0);
    std::vector<Gusting> first;
    for (int step = 0; step < steps; ++step) {
        const double seconds = 5.0 + (step * 2.0 * std::numbers::pi / turning / steps);
        const std::vector<Gusting> found =
            gusting_at(*device, eye, DVec3{0.0}, points, speed, seconds);
        for (std::size_t at = 0; at < points.size(); ++at) {
            mean[at] += found[at].sheen / steps;
            least[at] = std::min(least[at], found[at].sheen);
            most[at] = std::max(most[at], found[at].sheen);
        }
        if (step == 0) {
            first = found;
        }
    }
    double swing = 0.0;
    std::vector<double> sheens;
    std::vector<double> laid;
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(mean[at], WithinAbs(first[at].laid, 0.01));
        swing += (most[at] - least[at]) / static_cast<double>(points.size());
        sheens.push_back(first[at].sheen);
        laid.push_back(first[at].laid);
    }
    // At any moment the field isn't swaying all one way, but in waves.
    INFO("swinging " << swing << " on the whole");
    CHECK(swing > 0.4);
    CHECK(mean_difference(sheens, laid) > 0.1);
    CHECK(std::ranges::minmax(sheens).max - std::ranges::minmax(sheens).min > 0.6);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bushes and trees are held over in the gusts and rock in waves, stiffer than grass",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const WorldPosition eye = ground.here + (ground.up * 1.7);
    const std::vector<WorldPosition> points = square(ground, 120.0, 4.0);
    constexpr double speed = 6.0;
    constexpr int steps = 24;
    std::vector<double> bush_least(points.size(), 1.0);
    std::vector<double> bush_most(points.size(), -1.0);
    std::vector<double> tree_least(points.size(), 1.0);
    std::vector<double> tree_most(points.size(), -1.0);
    std::vector<Gusting> first;
    for (int step = 0; step < steps; ++step) {
        const double seconds = 5.0 + (step * 0.25);
        const std::vector<Gusting> found =
            gusting_at(*device, eye, DVec3{0.0}, points, speed, seconds);
        for (std::size_t at = 0; at < points.size(); ++at) {
            bush_least[at] = std::min(bush_least[at], found[at].bush);
            bush_most[at] = std::max(bush_most[at], found[at].bush);
            tree_least[at] = std::min(tree_least[at], found[at].tree);
            tree_most[at] = std::max(tree_most[at], found[at].tree);
        }
        if (step == 0) {
            first = found;
        }
    }
    // Held over furthest in the heart of a gust, for all they rock.
    double gusty_bush = 0.0;
    double lull_bush = 0.0;
    double gusty_tree = 0.0;
    double lull_tree = 0.0;
    int gusty = 0;
    int lulls = 0;
    for (std::size_t at = 0; at < points.size(); ++at) {
        const double bush = (bush_least[at] + bush_most[at]) / 2.0;
        const double tree = (tree_least[at] + tree_most[at]) / 2.0;
        if (first[at].gust > 0.9) {
            gusty_bush += bush;
            gusty_tree += tree;
            ++gusty;
        } else if (first[at].gust < 0.1) {
            lull_bush += bush;
            lull_tree += tree;
            ++lulls;
        }
        // Stiffer than grass, trees stiffer still, and never blown back upwind.
        const double gusted = first[at].speed * first[at].speed;
        CHECK(bush_most[at] < 0.3 * gusted / (gusted + 36.0));
        CHECK(tree_most[at] < bush_most[at]);
        CHECK(tree_least[at] > -0.03);
        CHECK(bush_most[at] - bush_least[at] > 0.01);
    }
    REQUIRE(gusty > 0);
    REQUIRE(lulls > 0);
    INFO("bushes " << gusty_bush / gusty << " in the gusts, " << lull_bush / lulls << " in lulls");
    INFO("trees " << gusty_tree / gusty << " in the gusts, " << lull_tree / lulls << " in lulls");
    CHECK(gusty_bush / gusty > 2.0 * (lull_bush / lulls));
    CHECK(gusty_tree / gusty > 2.0 * (lull_tree / lulls));
    // At any moment they aren't all rocking one way, but in waves.
    std::vector<double> bushes;
    std::vector<double> trees;
    for (const Gusting& found : first) {
        bushes.push_back(found.bush);
        trees.push_back(found.tree);
    }
    CHECK(std::ranges::minmax(bushes).max - std::ranges::minmax(bushes).min > 0.05);
    CHECK(std::ranges::minmax(trees).max - std::ranges::minmax(trees).min > 0.02);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("grass grows thicker and thinner in patches fixed to the ground", "[render][gusts]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Ground ground;
    std::vector<WorldPosition> points;
    for (int i = 0; i < 40; ++i) {
        for (int j = 0; j < 40; ++j) {
            points.push_back(ground.at(i * 5.0, j * 5.0));
        }
    }
    // Wherever the eye is, as the ground's anchor has it; the gusts' drift doesn't move them.
    const auto from = gusting_at(*device, ground.at(-30.0, 10.0), DVec3{0.0}, points, 0.0);
    const auto moved = gusting_at(*device, ground.at(150.0, 60.0), DVec3{0.0}, points, 0.0);
    double thinnest = 10.0;
    double thickest = 0.0;
    int thick_patches = 0;
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(moved[at].thick, WithinAbs(from[at].thick, 1e-3));
        thinnest = std::min(thinnest, from[at].thick);
        thickest = std::max(thickest, from[at].thick);
        thick_patches += from[at].thick > 2.0 ? 1 : 0;
        CHECK(from[at].taller >= 1.0);
        CHECK(from[at].taller <= 1.35 + 1e-4);
        CHECK((from[at].taller > 1.0) == (from[at].thick > 1.0));
    }
    // Thin and thick both, over the 200 m square, and thick in patches rather than everywhere.
    CHECK(thinnest < 0.9);
    CHECK(thickest > 2.5);
    CHECK(thick_patches > 40);
    CHECK(thick_patches < 800);
}
