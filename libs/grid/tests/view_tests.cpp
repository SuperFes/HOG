#include <hog/grid/grid.hpp>
#include <hog/grid/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using hog::grid::Key;
using hog::grid::View;

TEST_CASE("a page is seen as deep as its nodes span the pixels allowed", "[grid][view]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    const View view;
    const Key page{.depth = 5, .x = 3, .y = 4, .z = 5};
    CHECK(view.levels_at(grid, page, 0.0) == 6);
    // Nodes two levels down span the pixels allowed from here, so no deeper is wanted.
    const double two = grid.size(7) / view.angle;
    CHECK(view.levels_at(grid, page, two) == 2);
    CHECK(view.levels_at(grid, page, two * 0.99) == 3);
    CHECK(view.levels_at(grid, page, 1e9) == 1);
    CHECK(view.coarse(grid, 5, two));
    CHECK_FALSE(view.coarse(grid, 7, two));
    // Half the pixels: as deep from twice as far.
    CHECK(View{4.0}.levels_at(grid, page, two * 2.0) == 2);
}

TEST_CASE("pages under an open leaf are seen where coarse, caves and doubt only near",
          "[grid][view]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const View view;
    const Key leaf{.depth = 3, .x = 1, .y = 2, .z = 3};
    const std::uint8_t foot = 5;
    const auto air = hog::grid::Surface::open();
    const auto ground = hog::grid::Surface::plane(hog::DVec3{0.0, 0.0, 1.0}, 0.1);
    // From inside the leaf, every foot under it is near enough for its nodes to look coarse.
    const hog::DVec3 inside = grid.centre(leaf);
    std::vector<Key> pages;
    view.under(grid, Key{.depth = foot, .x = 4, .y = 8, .z = 12}, foot, air, inside, pages);
    CHECK(pages.size() == 1);
    pages.clear();
    // Above the foot or in the solid, only within the finest nodes' reach: none from far off.
    const hog::DVec3 off = inside + hog::DVec3{400.0, 0.0, 0.0};
    view.under(grid, leaf, foot, air, off, pages);
    CHECK(pages.empty());
    // But a leaf the ground runs through is only as coarse as its plane, so followed from afar.
    view.under(grid, leaf, foot, ground, off, pages);
    CHECK_FALSE(pages.empty());
    pages.clear();
    view.under(grid, leaf, foot, hog::grid::Surface::solid(), inside, pages);
    for (const Key page : pages) {
        CHECK(page.depth == foot);
        CHECK(leaf.contains(page));
        CHECK(grid.distance(page, inside) <= grid.finest() / view.angle);
    }
    CHECK_FALSE(pages.empty());
}
