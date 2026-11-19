#include <hog/grid/z_order.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using hog::grid::Key;
using hog::grid::ZMap;

TEST_CASE("nodes in Z order come each just before those under it", "[grid][z_order]") {
    const Key node{.depth = 3, .x = 5, .y = 2, .z = 6};
    CHECK(hog::grid::aligned(node.child(0)) == hog::grid::aligned(node));
    CHECK(hog::grid::aligned_end(node.child(7)) == hog::grid::aligned_end(node));
    CHECK(hog::grid::ZOrder{}(node, node.child(0)));
    CHECK(hog::grid::ZOrder{}(node.child(7).child(7), node.beside(1, 0, 0).value()));
    CHECK_FALSE(hog::grid::ZOrder{}(node, node));
}

TEST_CASE("the entries nested with a node are those over it, at it and under it",
          "[grid][z_order]") {
    const Key node{.depth = 3, .x = 5, .y = 2, .z = 6};
    ZMap<int> map;
    map[Key{}] = 0;
    map[node.parent()] = 1;
    map[node] = 2;
    map[node.child(0)] = 3;
    map[node.child(5).child(7)] = 4;
    // Beside it, under its parent, and the parent's first child's corner.
    map[node.beside(1, 0, 0).value()] = 5;
    map[node.parent().child(0)] = 6;
    map[node.parent().child(0).child(7)] = 7;

    std::vector<int> found;
    hog::grid::for_nested(map, node, [&](Key, int value) { found.push_back(value); });
    CHECK(found == std::vector<int>{0, 1, 2, 3, 4});

    found.clear();
    hog::grid::for_nested(map, node.child(5), [&](Key, int value) { found.push_back(value); });
    CHECK(found == std::vector<int>{0, 1, 2, 4});

    found.clear();
    hog::grid::for_nested(map, Key{}, [&](Key, int value) { found.push_back(value); });
    CHECK(found.size() == map.size());
}
