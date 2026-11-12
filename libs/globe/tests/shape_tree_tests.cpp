#include <hog/globe/shape_tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

using hog::DVec3;
using hog::globe::Ball;
using hog::globe::Box;
using hog::globe::HalfSpace;
using hog::globe::Inside;
using hog::globe::Material;
using hog::globe::NodeKey;
using hog::globe::Primitive;
using hog::globe::ShapeTree;
using hog::globe::Summary;

namespace {

constexpr double radius = 6'371'000.0;

// Where the crag stands: out of the ground, and two ways across it.
struct Frame {
    DVec3 up = glm::normalize(DVec3{0.3, 0.4, 0.86});
    DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    DVec3 north = glm::cross(up, east);
    DVec3 ground = up * radius;
};

// The planet's rock, a crag of basalt standing half out of it and a cave under it.
std::vector<Primitive> world() {
    const auto [up, east, north, ground] = Frame{};
    return {{.form = Ball{.middle = DVec3{0.0}, .radius = radius}, .material = Material::Granite},
            {.form = Box{.middle = ground + (up * 10.0),
                         .axes = {east, north, up},
                         .half = DVec3{20.0, 10.0, 15.0}},
             .material = Material::Basalt},
            {.form = Ball{.middle = ground - (up * 60.0), .radius = 25.0}, .carves = true}};
}

// Points within `within` metres of the crag, the same ones every run.
std::vector<DVec3> points_near(double within, std::size_t count) {
    const auto [up, east, north, ground] = Frame{};
    std::mt19937_64 random{7};
    std::uniform_real_distribution<double> across{-within, within};
    std::vector<DVec3> points;
    points.reserve(count);
    for (std::size_t n = 0; n < count; ++n) {
        points.push_back(ground + (east * across(random)) + (north * across(random)) +
                         (up * across(random)));
    }
    return points;
}

bool same(const Inside& one, const Inside& other) {
    return one.distance == other.distance && one.material == other.material;
}

} // namespace

TEST_CASE("primitives' distances", "[globe][shape_tree]") {
    using hog::globe::distance;
    CHECK(distance(Ball{.middle = DVec3{1.0, 0.0, 0.0}, .radius = 2.0}, DVec3{4.0, 0.0, 0.0}) ==
          1.0);
    CHECK(distance(HalfSpace{.normal = DVec3{0.0, 0.0, 1.0}, .offset = 3.0},
                   DVec3{5.0, 5.0, 1.0}) == -2.0);
    const Box box{.half = DVec3{1.0, 2.0, 3.0}};
    CHECK(distance(box, DVec3{0.0, 0.0, 0.0}) == -1.0);
    CHECK(distance(box, DVec3{4.0, 0.0, 0.0}) == 3.0);
    CHECK(std::abs(distance(box, DVec3{4.0, 6.0, 0.0}) - 5.0) < 1e-12);
}

TEST_CASE("a point reads the same from its node as from every primitive", "[globe][shape_tree]") {
    const auto [up, east, north, ground] = Frame{};
    for (const std::size_t most : {std::size_t{1}, std::size_t{4}}) {
        const ShapeTree tree{
            world(), radius - 500.0, radius + 500.0, {.smallest = 2.0, .primitives = most}};
        for (const DVec3& point : points_near(120.0, 4000)) {
            CHECK(same(tree.read(point), tree.at(point)));
            // Through any node holding it, at any level.
            for (int level = 0; level <= hog::globe::Cell::max_level; level += 3) {
                CHECK(same(tree.read(tree.key_under(point, level), point), tree.at(point)));
            }
        }
        CHECK(tree.read(ground - (up * 60.0)).material == Material::Air);
        CHECK(tree.read(ground + (up * 20.0)).material == Material::Basalt);
        CHECK(tree.read(ground - (up * 200.0)).material == Material::Granite);
    }
}

TEST_CASE("a node far from the crag keeps only the planet's rock", "[globe][shape_tree]") {
    const auto [up, east, north, ground] = Frame{};
    const ShapeTree tree{
        world(), radius - 500.0, radius + 500.0, {.smallest = 2.0, .primitives = 1}};
    const DVec3 far = glm::normalize(ground + (east * 5'000.0)) * radius;
    const NodeKey key = tree.built_over(tree.key_under(far, 20));
    CHECK(tree.primitives_in(key).size() == 1);
    // Away from the crag the tree stays shallow: it's split only where more than the planet's
    // rock matters.
    CHECK(key.level < 15);
    CHECK(tree.nodes() < 500'000);
    // High over the ground a node is all open.
    const Summary& sky = tree.summary(tree.key_under(up * (radius + 450.0), 20));
    CHECK(sky.all_open());
    CHECK(sky.solid() == 0.0);
    CHECK_FALSE(sky.estimated);
}

TEST_CASE("the tree's summaries add up from its leaves", "[globe][shape_tree]") {
    const ShapeTree tree{
        world(), radius - 500.0, radius + 500.0, {.smallest = 2.0, .primitives = 1}};
    double volume = 0.0;
    double solid = 0.0;
    double roots_solid = 0.0;
    double estimated = 0.0;
    for (const NodeKey leaf : tree.leaves()) {
        const Summary& summary = tree.summary(leaf);
        CHECK(tree.built_over(leaf) == leaf);
        volume += summary.volume;
        solid += summary.solid();
        estimated += summary.estimated ? summary.volume : 0.0;
    }
    for (std::uint8_t face = 0; face < 6; ++face) {
        roots_solid += tree.summary({.face = face}).solid();
    }
    const double shell = 4.0 / 3.0 * std::numbers::pi *
                         (std::pow(radius + 500.0, 3.0) - std::pow(radius - 500.0, 3.0));
    CHECK(std::abs(volume - shell) < shell * 1e-9);
    CHECK(std::abs(solid - roots_solid) < roots_solid * 1e-12);
    WARN(estimated << " m³ of " << shell << " guessed");
}

TEST_CASE("split until decided, a ball's volume is known to within the guessed nodes",
          "[globe][shape_tree]") {
    const auto [up, east, north, ground] = Frame{};
    constexpr double ball = 50.0;
    const ShapeTree tree{{{.form = Ball{.middle = ground, .radius = ball}}},
                         radius - 200.0,
                         radius + 200.0,
                         {.smallest = 1.0, .primitives = 0}};
    double solid = 0.0;
    double estimated = 0.0;
    for (const NodeKey leaf : tree.leaves()) {
        const Summary& summary = tree.summary(leaf);
        solid += summary.solid();
        estimated += summary.estimated ? summary.volume : 0.0;
    }
    const double truth = 4.0 / 3.0 * std::numbers::pi * ball * ball * ball;
    CHECK(std::abs(solid - truth) <= estimated);
    CHECK(estimated < 0.1 * truth);
    CHECK(std::abs(solid - truth) < 0.01 * truth);
    WARN("solid " << solid << " of " << truth << ", " << estimated << " m³ guessed in "
                  << tree.nodes() << " nodes");
}
