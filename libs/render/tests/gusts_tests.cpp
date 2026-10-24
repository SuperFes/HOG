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
using hog::render::gust_period;
using hog::render::gust_stretch;
using hog::render::Gusts;
using hog::render::GustsSeen;
using hog::render::sway_wavelength;
using hog::render::testing::probed;
using hog::render::testing::Probed;
using hog::render::testing::test_device;

namespace {

constexpr double radius = 115'000.0;
constexpr double turn = 2.0 * std::numbers::pi;

// A place on the ground, and east and north along it.
struct Ground {
    DVec3 up = glm::normalize(DVec3{0.3, -0.5, 0.8});
    DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    DVec3 north = glm::cross(up, east);
    WorldPosition here{up * radius};
    WorldPosition eye = here + (up * 1.7);

    [[nodiscard]] WorldPosition at(double east_of, double north_of) const {
        return here + (east * east_of) + (north * north_of);
    }

    // The gusts seen from `from`, the wind having blown east.
    [[nodiscard]] GustsSeen blowing_east(const WorldPosition& from, double speed = 6.0) const {
        Gusts gusts;
        gusts.blow(from, east * speed, up, 0.0);
        return gusts.seen();
    }
};

// How far the gusts' field has moved from `from` to `to`, the nearer way round its period.
DVec3 moved_by(DVec3 from, DVec3 to) {
    DVec3 by = to - from;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        by[axis] -= gust_period * std::round(by[axis] / gust_period);
    }
    return by;
}

// How far round one way from another an angle is, -pi to pi.
double round_by(double from, double to) {
    const double by = std::fmod(to - from, turn);
    return by - (turn * std::round(by / turn));
}

struct Gusting {
    double gust = 0.0;   // how hard the gusts blow, 0 to 1
    double speed = 0.0;  // m/s
    double laid = 0.0;   // how much further grass is laid over than on the whole
    double sheen = 0.0;  // and swaying with it
    double bush = 0.0;   // how far a bush's top is held over, of its height
    double tree = 0.0;   // and a tree's
    double thick = 0.0;  // times its density grass grows
    double taller = 0.0; // times its height grass stands for it
};

// How hard the gusts blow at each point, how fast the wind blows there and how it lays grass over,
// seen from `eye` with the gusts as `seen` has them, the wind blowing `speed` on the whole and the
// plants swayed for `seconds`.
std::vector<Gusting> gusting_at(hog::render::Device& device, const Ground& ground,
                                const WorldPosition& eye, const GustsSeen& seen,
                                std::span<const WorldPosition> points, double speed,
                                double seconds = 0.0) {
    const glm::vec3 anchor{seen.anchor};
    const glm::vec3 along{seen.along};
    const glm::vec3 up{ground.up};
    const glm::vec3 fixed{gust_anchor(eye, DVec3{0.0})};
    std::vector<Probed> asked;
    for (const WorldPosition& point : points) {
        const glm::vec3 from_eye{point - eye};
        asked.push_back({from_eye.x, from_eye.y, from_eye.z, static_cast<float>(speed)});
        asked.push_back({anchor.x, anchor.y, anchor.z, static_cast<float>(seconds)});
        asked.push_back({along.x, along.y, along.z, static_cast<float>(seen.wave)});
        asked.push_back({up.x, up.y, up.z, 0.0F});
        asked.push_back({fixed.x, fixed.y, fixed.z, 0.0F});
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

std::vector<double> gusts_at(hog::render::Device& device, const Ground& ground,
                             const WorldPosition& eye, const GustsSeen& seen,
                             std::span<const WorldPosition> points) {
    std::vector<double> gusts;
    for (const Gusting& found : gusting_at(device, ground, eye, seen, points, 0.0)) {
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
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 0.0);
    const GustsSeen before = gusts.seen();
    CHECK(glm::length(before.along - ground.east) < 1e-9);
    CHECK(before.wind == 4.0);
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 1.0);
    const DVec3 drifted = moved_by(gusts.seen().anchor, before.anchor);
    CHECK_THAT(drifted.x * gust_stretch, WithinAbs(6.0, 1e-6));
    CHECK_THAT(drifted.y, WithinAbs(0.0, 1e-9));
    CHECK(drifted.z > 0.0);
    // Still air carries them nowhere.
    Gusts still;
    still.blow(ground.eye, DVec3{0.0}, ground.up, 0.0);
    const GustsSeen calm = still.seen();
    still.blow(ground.eye, DVec3{0.0}, ground.up, 30.0);
    CHECK(still.seen().anchor == calm.anchor);
    CHECK(still.seen().wind == 0.0);
    CHECK_THAT(glm::dot(calm.along, ground.up), WithinAbs(0.0, 1e-9));
    CHECK_THAT(glm::length(calm.along), WithinAbs(1.0, 1e-9));
}

TEST_CASE("the gusts' field follows the eye in its own axes, within its period", "[render]") {
    const Ground ground;
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 0.0);
    const GustsSeen before = gusts.seen();
    gusts.blow(ground.eye + (ground.east * 10.0) + (ground.north * 5.0) + (ground.up * 2.0),
               ground.east * 4.0, ground.up, 0.0);
    const DVec3 followed = moved_by(before.anchor, gusts.seen().anchor);
    CHECK_THAT(followed.x, WithinAbs(10.0 / gust_stretch, 1e-6));
    CHECK_THAT(followed.y, WithinAbs(5.0, 1e-6));
    CHECK_THAT(followed.z, WithinAbs(2.0, 1e-6));
    for (const DVec3 by :
         {DVec3{-115'000.0, 20.5, 3'000.25}, DVec3{40'000.0, -90'000.0, 64'000.0}}) {
        gusts.blow(ground.eye + by, ground.east * 4.0, ground.up, 1'000.0);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            CHECK(gusts.seen().anchor[axis] >= 0.0);
            CHECK(gusts.seen().anchor[axis] < gust_period);
        }
        CHECK(gusts.seen().wave >= 0.0);
        CHECK(gusts.seen().wave < turn);
    }
}

TEST_CASE("the gusts' axes veer slowly round to a new wind", "[render]") {
    const Ground ground;
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 0.0);
    gusts.blow(ground.eye, ground.north * 4.0, ground.up, 1.0);
    CHECK(glm::dot(gusts.seen().along, ground.east) > 0.9);
    for (int second = 0; second < 100; ++second) {
        gusts.blow(ground.eye, (ground.north * 4.0) + (ground.up * 1.0), ground.up, 1.0);
        CHECK_THAT(glm::length(gusts.seen().along), WithinAbs(1.0, 1e-9));
        CHECK_THAT(glm::dot(gusts.seen().along, ground.up), WithinAbs(0.0, 1e-9));
    }
    CHECK(glm::dot(gusts.seen().along, ground.north) > 0.99);
    // And hold their last way through a calm.
    gusts.blow(ground.eye, DVec3{0.0}, ground.up, 60.0);
    CHECK(glm::dot(gusts.seen().along, ground.north) > 0.99);
}

TEST_CASE("the grass's sway runs downwind in waves as fast as the gusts", "[render]") {
    const Ground ground;
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 0.0);
    const double before = gusts.seen().wave;
    // Downwind a whole wave, it's as far into its sway; across the wind, no further.
    gusts.blow(ground.eye + (ground.east * sway_wavelength) + (ground.north * 7.0),
               ground.east * 4.0, ground.up, 0.0);
    CHECK_THAT(round_by(before, gusts.seen().wave), WithinAbs(0.0, 1e-6));
    gusts.blow(ground.eye + (ground.east * 3.0), ground.east * 4.0, ground.up, 0.0);
    CHECK_THAT(round_by(before, gusts.seen().wave), WithinAbs(turn / 4.0, 1e-6));
    // Carried downwind at 1.5 times the wind's 4 m/s, the wave the eye was on is 3 m further on
    // half a second later, and so is the eye.
    gusts.blow(ground.eye + (ground.east * 6.0), ground.east * 4.0, ground.up, 0.5);
    CHECK_THAT(round_by(before, gusts.seen().wave), WithinAbs(turn / 4.0, 1e-6));
}

TEST_CASE("gusts lie on the ground, the same seen from anywhere", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    const WorldPosition flying =
        ground.eye + (ground.east * 300.0) - (ground.north * 150.0) + (ground.up * 40.0);
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * 4.0, ground.up, 0.0);
    const std::vector<double> seen = gusts_at(*device, ground, ground.eye, gusts.seen(), points);
    gusts.blow(flying, ground.east * 4.0, ground.up, 0.0);
    const std::vector<double> from_afar = gusts_at(*device, ground, flying, gusts.seen(), points);
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(from_afar[at], WithinAbs(seen[at], 0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gusts drift downwind", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    const GustsSeen before = ground.blowing_east(ground.eye);
    GustsSeen after = before;
    after.anchor.x -= 60.0 / gust_stretch;
    const DVec3 blown = ground.east * 60.0;
    const std::vector<double> was = gusts_at(*device, ground, ground.eye, before, points);
    const std::vector<double> is =
        gusts_at(*device, ground, ground.eye, after, moved(points, blown));
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(is[at], WithinAbs(was[at], 0.01));
    }
    // Rather than standing still.
    CHECK(mean_difference(was, gusts_at(*device, ground, ground.eye, after, points)) > 0.1);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the gusts' field shows no seam where its period wraps", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 200.0, 5.0);
    // Lying just short of its period from the eye along each axis, then just past it.
    GustsSeen before = ground.blowing_east(ground.eye);
    before.anchor = DVec3{gust_period - 20.0};
    GustsSeen after = before;
    after.anchor = DVec3{20.0};
    const DVec3 by =
        (ground.east * 40.0 * gust_stretch) + (ground.north * 40.0) + (ground.up * 40.0);
    const std::vector<double> was = gusts_at(*device, ground, ground.eye, before, points);
    const std::vector<double> is = gusts_at(*device, ground, ground.eye, after, moved(points, -by));
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(is[at], WithinAbs(was[at], 0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gusts blow in patches tens of metres across and longer downwind, with lulls between",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const GustsSeen seen = ground.blowing_east(ground.eye);
    const std::vector<WorldPosition> points = square(ground, 600.0, 3.0);
    const auto shifted = [&](DVec3 by) {
        return gusts_at(*device, ground, ground.eye, seen, moved(points, by));
    };
    const std::vector<double> field = shifted(DVec3{0.0});
    const auto share = [&](auto&& kept) {
        return static_cast<double>(std::ranges::count_if(field, kept)) /
               static_cast<double>(field.size());
    };
    const double gusting = share([](double gust) { return gust > 0.75; });
    const double lull = share([](double gust) { return gust < 0.25; });
    const double metre = mean_difference(field, shifted(ground.east));
    const double near = mean_difference(field, shifted(ground.north * 5.0));
    const double apart = mean_difference(field, shifted(ground.north * 60.0));
    const double downwind = mean_difference(field, shifted(ground.east * 15.0));
    const double across = mean_difference(field, shifted(ground.north * 15.0));
    INFO("gusting " << gusting << ", lull " << lull << "; differing 1 m apart " << metre << ", 5 m "
                    << near << ", 60 m " << apart << "; 15 m downwind " << downwind << ", across "
                    << across);
    CHECK(gusting > 0.15);
    CHECK(gusting < 0.45);
    CHECK(lull > 0.3);
    CHECK(lull < 0.7);
    CHECK(metre < 0.05);
    CHECK(near < 0.5 * apart);
    CHECK(downwind < 0.7 * across);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gustier stretches some hundreds of metres long come and go among calmer ones",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const GustsSeen seen = ground.blowing_east(ground.eye);
    // Squares 100 m across, along a line 4 km across the wind.
    std::vector<double> shares;
    for (int square_at = 0; square_at < 40; ++square_at) {
        const std::vector<WorldPosition> points =
            moved(square(ground, 100.0, 5.0), ground.north * (square_at * 100.0));
        const std::vector<double> field = gusts_at(*device, ground, ground.eye, seen, points);
        shares.push_back(static_cast<double>(std::ranges::count_if(
                             field, [](double gust) { return gust > 0.75; })) /
                         static_cast<double>(field.size()));
    }
    const auto [calmest, gustiest] = std::ranges::minmax(shares);
    INFO("gusting over " << calmest << " to " << gustiest << " of a square");
    CHECK(calmest < 0.1);
    CHECK(gustiest > 0.5);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the wind slackens in the lulls and blows harder in the gusts, as hard on the whole",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const GustsSeen seen = ground.blowing_east(ground.eye);
    const std::vector<WorldPosition> points = square(ground, 1'200.0, 6.0);
    constexpr double speed = 6.0;
    double sum = 0.0;
    for (const Gusting& found : gusting_at(*device, ground, ground.eye, seen, points, speed)) {
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
    for (const Gusting& found : gusting_at(*device, ground, ground.eye, seen, points, 0.0)) {
        CHECK(found.speed == 0.0);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("grass is laid further over in the gusts and stands up in the lulls", "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const GustsSeen seen = ground.blowing_east(ground.eye);
    const std::vector<WorldPosition> points = square(ground, 600.0, 3.0);
    for (const Gusting& found : gusting_at(*device, ground, ground.eye, seen, points, 6.0)) {
        if (found.gust < 0.1) {
            CHECK(found.laid < -0.15);
        }
        if (found.gust > 0.9) {
            CHECK(found.laid > 0.1);
        }
    }
    for (const Gusting& found : gusting_at(*device, ground, ground.eye, seen, points, 0.0, 3.0)) {
        CHECK(found.laid == 0.0);
        CHECK(found.sheen == 0.0);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("grass sways about how far the gusts lay it over, in waves running through the gusts",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 120.0, 4.0);
    constexpr double speed = 6.0;
    // A whole sway, in even steps through it.
    constexpr int steps = 16;
    GustsSeen seen = ground.blowing_east(ground.eye, speed);
    std::vector<double> mean(points.size(), 0.0);
    std::vector<double> least(points.size(), 1.0);
    std::vector<double> most(points.size(), -1.0);
    std::vector<Gusting> first;
    for (int step = 0; step < steps; ++step) {
        seen.wave = step * turn / steps;
        const std::vector<Gusting> found =
            gusting_at(*device, ground, ground.eye, seen, points, speed);
        for (std::size_t at = 0; at < points.size(); ++at) {
            mean[at] += found[at].sheen / steps;
            least[at] = std::min(least[at], found[at].sheen);
            most[at] = std::max(most[at], found[at].sheen);
        }
        if (step == 0) {
            first = found;
        }
    }
    double gusty_swing = 0.0;
    double lull_swing = 0.0;
    int gusty = 0;
    int lulls = 0;
    std::vector<double> sheens;
    std::vector<double> laid;
    for (std::size_t at = 0; at < points.size(); ++at) {
        CHECK_THAT(mean[at], WithinAbs(first[at].laid, 0.01));
        if (first[at].gust > 0.9) {
            gusty_swing += most[at] - least[at];
            ++gusty;
        } else if (first[at].gust < 0.1) {
            lull_swing += most[at] - least[at];
            ++lulls;
        }
        sheens.push_back(first[at].sheen);
        laid.push_back(first[at].laid);
    }
    REQUIRE(gusty > 0);
    REQUIRE(lulls > 0);
    gusty_swing /= gusty;
    lull_swing /= lulls;
    INFO("swinging " << gusty_swing << " in the gusts, " << lull_swing << " in the lulls");
    CHECK(gusty_swing > 0.5);
    CHECK(lull_swing < 0.4 * gusty_swing);
    // At any moment the field isn't swaying all one way, but in waves.
    CHECK(mean_difference(sheens, laid) > 0.05);
    CHECK(std::ranges::minmax(sheens).max - std::ranges::minmax(sheens).min > 0.6);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the grass's waves run downwind with the gusts, rather than rising and falling in place",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const std::vector<WorldPosition> points = square(ground, 120.0, 4.0);
    constexpr double speed = 6.0;
    constexpr double seconds = 0.2;
    Gusts gusts;
    gusts.blow(ground.eye, ground.east * speed, ground.up, 0.0);
    const std::vector<Gusting> before =
        gusting_at(*device, ground, ground.eye, gusts.seen(), points, speed);
    gusts.blow(ground.eye, ground.east * speed, ground.up, seconds);
    const auto sheens = [](const std::vector<Gusting>& found) {
        std::vector<double> sheen;
        sheen.reserve(found.size());
        for (const Gusting& at : found) {
            sheen.push_back(at.sheen);
        }
        return sheen;
    };
    // Carried 1.5 times the wind.
    const double carried = 1.5 * speed * seconds;
    const std::vector<double> followed = sheens(gusting_at(
        *device, ground, ground.eye, gusts.seen(), moved(points, ground.east * carried), speed));
    const std::vector<double> in_place =
        sheens(gusting_at(*device, ground, ground.eye, gusts.seen(), points, speed));
    const double travelled = mean_difference(sheens(before), followed);
    const double changed = mean_difference(sheens(before), in_place);
    INFO("following the wind " << travelled << ", in place " << changed);
    CHECK(travelled < 0.2 * changed);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bushes and trees are held over in the gusts and rock in waves, stiffer than grass",
          "[render][gpu]") {
    const auto device = test_device();
    const Ground ground;
    const GustsSeen seen = ground.blowing_east(ground.eye);
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
            gusting_at(*device, ground, ground.eye, seen, points, speed, seconds);
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
    // Wherever the eye is, and wherever the gusts.
    const auto from = gusting_at(*device, ground, ground.at(-30.0, 10.0), {}, points, 0.0);
    const auto moved = gusting_at(*device, ground, ground.at(150.0, 60.0), {}, points, 0.0);
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
