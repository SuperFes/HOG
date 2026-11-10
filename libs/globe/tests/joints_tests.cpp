#include "../src/joints.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using hog::DVec3;
using hog::globe::Block;
using hog::globe::block_at;
using hog::globe::closest_joints;
using hog::globe::Core;
using hog::globe::core_over;
using hog::globe::joint_spacing;
using hog::globe::rotted_by;
using hog::globe::widely_jointed;
using hog::globe::widest_joints;

namespace {

// Points scattered through a stretch of rock far from the centre, as on a planet's surface.
std::vector<DVec3> scattered(int count) {
    std::vector<DVec3> points;
    for (int n = 0; n < count; ++n) {
        const double k = n;
        points.push_back(DVec3{3.1e6, -2.2e6, 4.0e5} + DVec3{std::fmod(k * 7.31, 97.0),
                                                             std::fmod(k * 3.77, 89.0),
                                                             std::fmod(k * 5.13, 83.0)});
    }
    return points;
}

// Blocks of one domain cross nowhere.
bool overlap(const Block& a, const Block& b) {
    return a.origin == b.origin && a.low.x < b.high.x && b.low.x < a.high.x && a.low.y < b.high.y &&
           b.low.y < a.high.y && a.low.z < b.high.z && b.low.z < a.high.z;
}

} // namespace

TEST_CASE("joint spacing runs from closely jointed weak rock to widely jointed strong rock",
          "[globe][joints]") {
    CHECK(std::abs(joint_spacing(0.0) - closest_joints) < 1e-12);
    CHECK(std::abs(joint_spacing(1.0) - widely_jointed) < 1e-12);
    CHECK(joint_spacing(0.3) < joint_spacing(0.7));
}

TEST_CASE("blocks hold their points and never overlap", "[globe][joints]") {
    for (const double strength : {0.0, 0.5, 1.0}) {
        std::vector<Block> blocks;
        for (const DVec3& point : scattered(400)) {
            const Block block = block_at(point, strength, 11);
            CHECK(block.inset(point) >= 0.0);
            CHECK(block_at(point, strength, 11).id == block.id);
            blocks.push_back(block);
        }
        for (std::size_t a = 0; a < blocks.size(); ++a) {
            for (std::size_t b = a + 1; b < blocks.size(); ++b) {
                if (blocks[a].id == blocks[b].id) {
                    CHECK(blocks[a].low == blocks[b].low);
                    CHECK(blocks[a].high == blocks[b].high);
                } else {
                    CHECK_FALSE(overlap(blocks[a], blocks[b]));
                }
            }
        }
    }
}

TEST_CASE("blocks are as wide as the rock's joints are apart", "[globe][joints]") {
    for (const double strength : {0.0, 0.5, 1.0}) {
        double logs = 0.0;
        int sides = 0;
        for (const DVec3& point : scattered(600)) {
            const DVec3 size = block_at(point, strength, 5).size();
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                // The widest joints' nearest, split four times nearest a side.
                CHECK(size[axis] > 0.4 * widest_joints * std::pow(0.35, 4.0));
                CHECK(size[axis] < 1.7 * widest_joints);
                logs += std::log(size[axis]);
                ++sides;
            }
        }
        // Points fall in wide blocks more often than narrow ones, so their sides run wider than
        // the spacing, but within a split of it.
        const double typical = std::exp(logs / sides);
        CAPTURE(strength, typical);
        CHECK(typical > 0.8 * joint_spacing(strength));
        CHECK(typical < 2.0 * joint_spacing(strength));
    }
}

TEST_CASE("each domain of rock has its joints turned its own way", "[globe][joints]") {
    int turned = 0;
    for (const DVec3& point : scattered(400)) {
        const Block block = block_at(point, 0.5, 3);
        for (std::size_t a = 0; a < 3; ++a) {
            CHECK(std::abs(glm::length(block.sets[a]) - 1.0) < 1e-12);
            CHECK(std::abs(glm::dot(block.sets[a], block.sets[(a + 1) % 3])) < 1e-12);
        }
        // Its domain's middle the nearest, so no further off than a domain's corner.
        CHECK(glm::length(point - block.origin) < 1.6 * hog::globe::joint_domain);
        turned +=
            block_at(point + DVec3{3.0 * hog::globe::joint_domain, 0.0, 0.0}, 0.5, 3).sets[0] !=
                    block.sets[0]
                ? 1
                : 0;
    }
    CHECK(turned == 400);
}

TEST_CASE("faults break the rock along wandering traces, most of it whole", "[globe][joints]") {
    int broken = 0;
    int crushed = 0;
    constexpr int count = 4'000;
    for (int n = 0; n < count; ++n) {
        const DVec3 point = DVec3{3.1e6, -2.2e6, 4.0e5} +
                            DVec3{std::fmod(n * 37.1, 4'000.0), std::fmod(n * 91.7, 3'000.0), 0.0};
        const double fault = hog::globe::faulted(point, 9);
        CHECK(fault >= 0.0);
        CHECK(fault <= 1.0);
        broken += fault > 0.0 ? 1 : 0;
        crushed += fault > 0.5 ? 1 : 0;
    }
    // Damage zones a fiftieth of the blocks wide either side of each trace, wider where the
    // ground cuts a fault aslant.
    CHECK(broken > count / 50);
    CHECK(broken < count / 3);
    CHECK(crushed > 0);
}

TEST_CASE("a block's inset is the way to its nearest joint", "[globe][joints]") {
    const Block block{.low = {0.0, 0.0, 0.0},
                      .high = {4.0, 2.0, 6.0},
                      .id = 1,
                      .sets = {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}}};
    CHECK(block.inset({2.0, 1.0, 3.0}) == 1.0);
    CHECK(block.inset({3.5, 1.0, 3.0}) == 0.5);
    CHECK(block.inset({2.0, 1.0, 5.9}) < 0.1 + 1e-12);
}

TEST_CASE("a slab weathers as its thin side has it", "[globe][joints]") {
    const Block cube{.low = {0.0, 0.0, 0.0}, .high = {6.0, 6.0, 6.0}, .id = 1};
    const Block slab{.low = {0.0, 0.0, 0.0}, .high = {6.0, 6.0, 1.0}, .id = 4};
    CHECK(cube.breadth() == 6.0);
    CHECK(slab.breadth() < 2.5);
}

TEST_CASE("rock rots in from its joints the less the faster the ground brings it up",
          "[globe][joints]") {
    // Never worn, it rots as fast as it's made for as long as it's had.
    CHECK(std::abs(rotted_by(1e-5, 0.0, 2.0, 1e5) - 1.0) < 1e-9);
    // Brought up fast, it reaches the top having rotted only while it lay shallow.
    CHECK(std::abs(rotted_by(1e-5, 1e-3, 2.0, 1e7) - 0.005) < 1e-9);
    // Rotting more the longer it's had, never more than as fast as it's made.
    double last = 0.0;
    for (const double years : {1e3, 1e4, 1e5, 1e6, 1e7}) {
        const double rotted = rotted_by(1e-5, 1e-5, 2.0, years);
        CHECK(rotted > last);
        CHECK(rotted <= 1e-5 * years);
        last = rotted;
    }
    CHECK(rotted_by(0.0, 1e-5, 2.0, 1e6) == 0.0);
}

TEST_CASE("only blocks wider than twice their rotting leave cores, no higher than they rotted",
          "[globe][joints]") {
    const Block block{.low = {0.0, 0.0, 0.0},
                      .high = {6.0, 6.0, 6.0},
                      .id = 1,
                      .sets = {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}}};
    const DVec3 up{0.0, 0.0, 1.0};
    CHECK(core_over(block, 0.0, {3.0, 3.0, 2.0}, up).height == 0.0);
    CHECK(core_over(block, 3.5, {3.0, 3.0, 2.0}, up).height == 0.0);
    // Rotted a metre in, its core runs from 1 to 5 up, standing over ground at 2 by no more than
    // the metre stripped about it.
    const Core held = core_over(block, 1.0, {3.0, 3.0, 2.0}, up);
    CHECK(held.height == 1.0);
    CHECK(glm::length(held.rise) == 0.0);
    CHECK(held.breadth == 4.0);
    CHECK(std::abs(core_over(block, 1.0, {3.0, 3.0, 4.9}, up).height - 0.1) < 1e-6);
    CHECK(core_over(block, 1.0, {3.0, 3.0, 5.5}, up).height == 0.0);
}

TEST_CASE("a core rounds off toward its edges", "[globe][joints]") {
    const Block block{.low = {0.0, 0.0, 0.0},
                      .high = {12.0, 12.0, 12.0},
                      .id = 1,
                      .sets = {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}}};
    const DVec3 up{0.0, 0.0, 1.0};
    // Rotted 2 in, its top is flat 4 across and rounds off over 2 either side, from 10 up.
    CHECK(std::abs(core_over(block, 2.0, {6.0, 6.0, 9.0}, up).height - 1.0) < 1e-6);
    const Core edge = core_over(block, 2.0, {9.5, 6.0, 9.0}, up);
    CHECK(std::abs(edge.height - (std::sqrt(4.0 - (1.5 * 1.5)) - 1.0)) < 1e-6);
    const double step = 1e-4;
    const double slope = (core_over(block, 2.0, {9.5 + step, 6.0, 9.0}, up).height -
                          core_over(block, 2.0, {9.5 - step, 6.0, 9.0}, up).height) /
                         (2.0 * step);
    CHECK(std::abs(edge.rise.x - slope) < 1e-3);
    CHECK(std::abs(edge.rise.y) < 1e-9);
    CHECK(core_over(block, 2.0, {10.1, 6.0, 9.0}, up).height == 0.0);
}

TEST_CASE("a core standing clear of the ground settles onto it", "[globe][joints]") {
    const Block block{.low = {0.0, 0.0, 0.0},
                      .high = {12.0, 12.0, 12.0},
                      .id = 1,
                      .sets = {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}}};
    // Its core runs from 2 to 10 up; over ground at 1 it stands as high as it was stripped.
    const Core settled = core_over(block, 2.0, {6.0, 6.0, 1.0}, {0.0, 0.0, 1.0});
    CHECK(settled.height > 1.5);
    CHECK(settled.height <= 2.0);
}

TEST_CASE("turned blocks' cores stand no higher than they rotted", "[globe][joints]") {
    for (const DVec3& point : scattered(200)) {
        const Block block = block_at(point, 1.0, 7);
        const Core core = core_over(block, 0.8, point, glm::normalize(point));
        CHECK(core.height >= 0.0);
        CHECK(core.height <= 0.8);
        CHECK(std::isfinite(glm::length(core.rise)));
    }
}
