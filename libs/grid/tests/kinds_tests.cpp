#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Catch::Approx;
using hog::DVec3;
using hog::grid::Shape;
using hog::grid::Surface;
using hog::grid::Tree;

namespace {

constexpr std::uint8_t levels = 6;
constexpr double across = 1 << levels;

DVec3 middle(int x, int y, int z) {
    return DVec3{x + 0.5, y + 0.5, z + 0.5};
}

// The surface of a ball `radius` finest nodes round, centred at `centre`, through each finest
// node: its tangent plane there.
Tree<Shape> ball(DVec3 centre, double radius) {
    return Tree<Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 out = middle(x, y, z) - centre;
        return Surface::plane(glm::normalize(out), glm::length(out) - radius);
    });
}

// How far the finest node at (x, y, z) lies from the surface as the tree has it, in finest
// nodes: from the plane of the leaf over it.
double held_distance(const Tree<Shape>& tree, int x, int y, int z) {
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const auto uz = static_cast<std::uint32_t>(z);
    const auto leaf = tree.at(levels, ux, uy, uz);
    const auto shift = static_cast<unsigned>(levels - leaf.depth);
    const double side = std::ldexp(1.0, static_cast<int>(shift));
    const DVec3 leaf_middle =
        (DVec3{static_cast<double>(ux >> shift), static_cast<double>(uy >> shift),
               static_cast<double>(uz >> shift)} +
         0.5) *
        side;
    if (!leaf.value.crosses()) {
        return leaf.value.is_open() ? side : -side;
    }
    return (leaf.value.distance() * side) +
           glm::dot(leaf.value.normal(), middle(x, y, z) - leaf_middle);
}

} // namespace

TEST_CASE("a surface keeps its plane to within its storage", "[grid]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const DVec3 raw{static_cast<double>(hog::hash_values(1, n) % 2001) - 1000.0,
                        static_cast<double>(hog::hash_values(2, n) % 2001) - 1000.0,
                        static_cast<double>(hog::hash_values(3, n) % 2001) - 1000.0};
        if (glm::length(raw) < 1.0) {
            continue;
        }
        const DVec3 normal = glm::normalize(raw);
        const double distance = (static_cast<double>(hog::hash_values(4, n) % 1001) / 1000.0) - 0.5;
        const Surface surface = Surface::plane(normal, distance * 0.5);
        REQUIRE(surface.crosses());
        CHECK(glm::length(surface.normal() - normal) < 1e-4);
        CHECK(surface.distance() == Approx(distance * 0.5).margin(1e-7));
    }
}

TEST_CASE("a plane missing its node leaves it all solid or all open", "[grid]") {
    const DVec3 up{0.0, 0.0, 1.0};
    CHECK(Surface::plane(up, 0.6).is_open());
    CHECK(Surface::plane(up, -0.6).is_solid());
    CHECK(Surface::plane(up, 0.4).crosses());
    const DVec3 corner = glm::normalize(DVec3{1.0});
    CHECK(Surface::plane(corner, 0.8).crosses());
    CHECK(Surface::plane(corner, 0.9).is_open());
    CHECK(Surface{}.is_open());
    CHECK_FALSE(Surface::from_raw(Surface::solid().raw_distance(), 1, 0));
    CHECK(Surface::from_raw(Surface::solid().raw_distance(), 0, 0) == Surface::solid());
}

TEST_CASE("eight solid and open with no plane between sum up to one between them", "[grid]") {
    const Surface lower = Surface::solid();
    const Surface upper = Surface::open();
    const Surface summed = Shape::summary({lower, lower, lower, lower, upper, upper, upper, upper});
    REQUIRE(summed.crosses());
    CHECK(summed.normal().z == Approx(1.0).margin(1e-4));
    CHECK(summed.distance() == Approx(0.0).margin(1e-6));
    CHECK(Shape::summary({lower, lower, lower, lower, lower, lower, lower, lower}).is_solid());
}

TEST_CASE("flat ground however tilted is one leaf", "[grid]") {
    const DVec3 normal = glm::normalize(DVec3{1.0, 2.0, 3.0});
    const double through = glm::dot(normal, DVec3{across / 2});
    const auto flat = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        return Surface::plane(normal, glm::dot(normal, middle(x, y, z)) - through);
    });
    CHECK(flat.nodes() == 1);
    const Surface root = flat.root().value;
    REQUIRE(root.crosses());
    CHECK(glm::length(root.normal() - normal) < 1e-4);
    CHECK(root.distance() == Approx(0.0).margin(1e-6));
}

TEST_CASE("curved ground splits more the tighter it curves", "[grid]") {
    const auto tight = ball(DVec3{32.0, 32.0, -8.0}, 40.0);
    const auto gentle = ball(DVec3{32.0, 32.0, -1960.0}, 1992.0);
    CHECK(tight.nodes() > 1);
    CHECK(gentle.nodes() < tight.nodes());
    // Fewer leaves than the finest nodes the surface crosses.
    CHECK(tight.leaves() < static_cast<std::size_t>(across * across));
}

TEST_CASE("merged ground stays near the surface it was built from", "[grid]") {
    const DVec3 centre{32.0, 32.0, -8.0};
    const double radius = 40.0;
    const auto tree = ball(centre, radius);
    double worst = 0.0;
    for (int z = 0; z < 64; ++z) {
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const double truth = glm::length(middle(x, y, z) - centre) - radius;
                const auto leaf =
                    tree.at(levels, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                            static_cast<std::uint32_t>(z));
                if (leaf.value.crosses()) {
                    if (std::abs(truth) < 1.0) {
                        worst = std::max(worst, std::abs(held_distance(tree, x, y, z) - truth));
                    }
                } else if (std::abs(truth) > 1.0) {
                    CHECK(leaf.value.is_open() == (truth > 0.0));
                }
            }
        }
    }
    CHECK(worst < 2 * Shape::tolerance);
}

TEST_CASE("a crease is kept as fine as it is", "[grid]") {
    // A ridge along y: two planes meeting between nodes' sides.
    const DVec3 left = glm::normalize(DVec3{-1.0, 0.0, 1.0});
    const DVec3 right = glm::normalize(DVec3{1.0, 0.0, 1.0});
    const DVec3 top{31.3, 0.0, 32.0};
    const auto ridge = Tree<Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 at = middle(x, y, z);
        const DVec3 normal = at.x < top.x ? left : right;
        return Surface::plane(normal, glm::dot(normal, at - top));
    });
    CHECK(ridge.at(levels, 31, 10, 31).depth == levels);
    CHECK(ridge.at(levels, 8, 10, 8).depth < levels);
    const auto back = Tree<Shape>::decode(ridge.encode());
    REQUIRE(back);
    CHECK(back->same_as(ridge));
}
