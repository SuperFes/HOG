#include <hog/baked/node.hpp>
#include <hog/core/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

using hog::baked::key_at;
using hog::baked::key_of;
using hog::baked::last_key_in;
using hog::baked::Node;
using hog::baked::node_of;
using hog::baked::Point;

namespace {

std::uint32_t draw(std::uint64_t seed, std::uint64_t n, std::uint32_t below) {
    return static_cast<std::uint32_t>(hog::hash_values(seed, n) % below);
}

} // namespace

TEST_CASE("node keys come back as their nodes", "[baked]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const auto size = static_cast<std::uint8_t>(draw(1, n, 12));
        const std::uint32_t mask = ~((std::uint32_t{1} << size) - 1U);
        const Node node{.face = static_cast<std::uint8_t>(draw(2, n, 6)),
                        .size = size,
                        .x = draw(3, n, 1U << 20U) & mask,
                        .y = draw(4, n, 1U << 20U) & mask,
                        .z = draw(5, n, 1U << 20U) & mask};
        const auto key = key_of(node);
        REQUIRE(node_of(key) == node);
    }
}

TEST_CASE("node keys refuse what isn't a node", "[baked]") {
    auto key = key_of(Node{.face = 1, .size = 3, .x = 8, .y = 16, .z = 24});
    CHECK_FALSE(node_of(std::span{key}.first(4)));
    key.back() = std::byte{2};
    CHECK(node_of(key));
    // Off its own size's grid.
    key.back() = std::byte{5};
    CHECK_FALSE(node_of(key));
    key = key_of(Node{.face = 1});
    key.front() = std::byte{6};
    CHECK_FALSE(node_of(key));
}

TEST_CASE("a node's keys lie between its corner's and its last voxel's", "[baked]") {
    const Node node{.face = 2, .size = 4, .x = 32, .y = 48, .z = 16};
    const auto first = key_of(Node{.face = 2, .x = 32, .y = 48, .z = 16});
    const auto last = last_key_in(node);
    for (std::uint32_t k = 0; k < 16; ++k) {
        const Node inside{.face = 2, .size = 0, .x = 32 + k, .y = 48 + (15 - k), .z = 16 + k};
        CHECK(first <= key_of(inside));
        CHECK(key_of(inside) <= last);
    }
    CHECK(key_of(node) <= last);
    // Beside it, past it.
    CHECK(last < key_of(Node{.face = 2, .size = 4, .x = 48, .y = 48, .z = 16}));
}

TEST_CASE("the node holding a point is the greatest key no greater than the point's", "[baked]") {
    // Nodes tiling a cube 64 across unevenly: the whole split into eighths, one of those split
    // again, one of those split again.
    std::vector<Node> nodes;
    const Node whole{.face = 3, .size = 6};
    for (std::size_t a = 0; a < 8; ++a) {
        const Node eighth = whole.child(a);
        if (a != 5) {
            nodes.push_back(eighth);
            continue;
        }
        for (std::size_t b = 0; b < 8; ++b) {
            const Node finer = eighth.child(b);
            if (b != 2) {
                nodes.push_back(finer);
                continue;
            }
            for (std::size_t c = 0; c < 8; ++c) {
                nodes.push_back(finer.child(c));
            }
        }
    }
    std::vector<std::pair<hog::baked::Key, Node>> keys;
    keys.reserve(nodes.size());
    for (const Node& node : nodes) {
        keys.emplace_back(key_of(node), node);
    }
    std::ranges::sort(keys);
    for (std::uint64_t n = 0; n < 2000; ++n) {
        const Point p{.face = 3, .x = draw(6, n, 64), .y = draw(7, n, 64), .z = draw(8, n, 64)};
        const auto search = key_at(p);
        const auto after = std::ranges::upper_bound(keys, search, {},
                                                    [](const auto& entry) { return entry.first; });
        REQUIRE(after != keys.begin());
        const Node& found = std::prev(after)->second;
        REQUIRE(found.contains(p));
    }
}
