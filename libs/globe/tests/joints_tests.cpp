#include "../src/joints.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using hog::DVec3;
using hog::globe::Block;
using hog::globe::block_at;
using hog::globe::closest_joints;
using hog::globe::joint_spacing;
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

bool overlap(const Block& a, const Block& b) {
    return a.low.x < b.high.x && b.low.x < a.high.x && a.low.y < b.high.y && b.low.y < a.high.y &&
           a.low.z < b.high.z && b.low.z < a.high.z;
}

} // namespace

TEST_CASE("joint spacing runs from closely jointed weak rock to widely jointed strong rock",
          "[globe][joints]") {
    CHECK(std::abs(joint_spacing(0.0) - closest_joints) < 1e-12);
    CHECK(std::abs(joint_spacing(1.0) - widest_joints) < 1e-12);
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
                CHECK(size[axis] > 0.3 * closest_joints);
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

TEST_CASE("a block's inset is the way to its nearest joint", "[globe][joints]") {
    const Block block{.low = {0.0, 0.0, 0.0}, .high = {4.0, 2.0, 6.0}, .id = 1};
    CHECK(block.inset({2.0, 1.0, 3.0}) == 1.0);
    CHECK(block.inset({3.5, 1.0, 3.0}) == 0.5);
    CHECK(block.inset({2.0, 1.0, 5.9}) < 0.1 + 1e-12);
}
