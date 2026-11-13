// A waterfall running off a 7.5 m cliff: the bricks the flow changes each second, and what they
// cost to send as the store codes them. Run: hog_flow_tests "[.network]"
#include "block.hpp"

#include <hog/core/bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <map>
#include <print>

using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;
using hog::flow::testing::full;
using hog::flow::testing::tick;

TEST_CASE("a waterfall's cost on the network", "[.network]") {
    // Open ground to the right of a cliff and a plateau to the left, in a rim.
    const auto shape = [](std::uint32_t i, std::uint32_t j, int layer) {
        const bool rim = i > 0 && j > 0 && j + 1 < Block::side && i + 1 < Block::side;
        return rim && (i >= 12 || layer >= 15);
    };
    Block block{floor_above_sea(100.0), false, shape};
    block.region().flush();
    std::println("waterfall, a spring pouring at the cliff's lip; bricks changed and bytes to send "
                 "each window:");
    std::map<std::uint64_t, hog::globe::BrickSite> window;
    std::size_t total = 0;
    int steps_in_window = 0;
    const int per_second = static_cast<int>(1.0 / tick);
    for (int step = 0; step < 240 * per_second; ++step) {
        // The spring tops its voxel up each step, as a fed stream would.
        block.pour(11, 12, 15, full / 8);
        block.region().step(tick);
        for (const hog::globe::BrickSite site : block.region().flush()) {
            window.emplace(hog::globe::brick_key(site), site);
        }
        if (++steps_in_window == per_second * (step < 30 * per_second ? 5 : 30)) {
            std::size_t bytes = 0;
            for (const auto& [key, site] : window) {
                (void)key;
                hog::ByteWriter out;
                hog::globe::Brick::pack(block.world().brick(site)).write(out);
                bytes += std::move(out).take().size();
            }
            total += bytes;
            const double seconds = steps_in_window * tick;
            std::println(
                "  t={:3.0f} s: {} bricks changed in the last {:.0f} s, {} bytes ({:.0f} B/s)",
                (step + 1) * tick, window.size(), seconds, bytes,
                static_cast<double>(bytes) / seconds);
            window.clear();
            steps_in_window = 0;
        }
    }
    std::println("  total over 240 s: {} bytes", total);
    CHECK(total > 0);
}
