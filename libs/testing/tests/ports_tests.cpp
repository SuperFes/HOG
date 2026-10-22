#include <hog/testing/ports.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>

using hog::testing::random_port;

TEST_CASE("ports to listen on are drawn apart, below Windows' dynamic range", "[testing]") {
    std::set<std::uint16_t> drawn;
    for (int i = 0; i < hog::testing::port_tries; ++i) {
        const std::uint16_t port = random_port();
        CHECK(port >= hog::testing::lowest_port);
        CHECK(port <= hog::testing::highest_port);
        CHECK(port < 49'152);
        drawn.insert(port);
    }
    // Not a run of neighbours one reserved block could hold.
    CHECK(*drawn.rbegin() - *drawn.begin() > 200);
}
