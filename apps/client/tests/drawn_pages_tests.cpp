#include <hog/client/drawn_pages.hpp>
#include <hog/client/held_pages.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using hog::client::DrawnPage;
using hog::client::HeldPages;
using hog::grid::Key;
using hog::grid::Shape;
using hog::grid::Surface;
using hog::grid::Tree;

namespace {

std::vector<std::byte> coded(Key root, Tree<Shape> shape) {
    return hog::grid::Page{.root = root,
                           .generator = 1,
                           .shape = hog::grid::Held<Shape>{.version = 1, .tree = std::move(shape)},
                           .material = {},
                           .water = {}}
        .encode();
}

// A top page of air, open only along a row of its finest nodes, x from 16 to 23 at y = z = 16.
Tree<Shape> top_page() {
    return Tree<Shape>::grow(5,
                             [](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                std::uint32_t z) -> std::optional<Tree<Shape>::Leaf> {
                                 if (depth < 5) {
                                     return std::nullopt;
                                 }
                                 return Tree<Shape>::Leaf{.value = Surface::open(),
                                                          .open = x >= 16 && x < 24 && y == 16 &&
                                                                  z == 16};
                             });
}

const DrawnPage* find(const std::vector<DrawnPage>& drawn, Key page) {
    const auto found = std::ranges::find(drawn, page, &DrawnPage::page);
    return found == drawn.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("pages are drawn deeper and finer as the eye nears them", "[client][drawn_pages]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    HeldPages held{grid};
    REQUIRE(held.take(coded(Key{}, top_page())).has_value());
    for (std::uint32_t x = 16; x < 24; ++x) {
        const auto air = Tree<Shape>::build(6, [](int, int, int) { return Surface::open(); });
        REQUIRE(held.take(coded(Key{.depth = 5, .x = x, .y = 16, .z = 16}, air)).has_value());
    }
    const Key target{.depth = 5, .x = 20, .y = 16, .z = 16};

    // Far off, the top page alone, shallow.
    const auto far = hog::client::drawn_pages(held, grid.centre(target) + hog::DVec3{1e6, 0, 0});
    REQUIRE(far.size() == 1);
    CHECK(far.front().page == Key{});
    CHECK(far.front().levels == 1);
    CHECK(far.front().under.empty());

    // At the target, the top page whole, the target drawn in its place as deep as it goes.
    const auto at = hog::client::drawn_pages(held, grid.centre(target));
    const DrawnPage* top = find(at, Key{});
    REQUIRE(top != nullptr);
    CHECK(top->levels == 5);
    CHECK(std::ranges::find(top->under, target) != top->under.end());
    // A snapshot of the pages held draws them as they do.
    const hog::client::PagesSnapshot snapshot{grid, held.entries()};
    CHECK(hog::client::drawn_pages(snapshot, grid.centre(target)) == at);
    CHECK(snapshot.page(target) == held.page(target));
    CHECK(snapshot.page(Key{.depth = 5, .x = 1, .y = 1, .z = 1}) == nullptr);
    const DrawnPage* here = find(at, target);
    REQUIRE(here != nullptr);
    CHECK(here->levels == 6);
    CHECK(here->under.empty());
    // Each drawn page but the top is drawn in the place of the one over it.
    for (const DrawnPage& page : at) {
        if (page.page != Key{}) {
            CHECK(std::ranges::find(top->under, page.page) != top->under.end());
        }
    }

    // Along the way in, ever deeper and finer.
    std::uint8_t levels = 0;
    std::size_t pages = 0;
    for (int halved = 0; halved < 21; ++halved) {
        const double away = std::ldexp(1e6, -halved);
        CAPTURE(away);
        const auto drawn =
            hog::client::drawn_pages(held, grid.centre(target) + hog::DVec3{away, 0.0, 0.0});
        const DrawnPage* over = find(drawn, Key{});
        REQUIRE(over != nullptr);
        CHECK(over->levels >= levels);
        CHECK(drawn.size() >= pages);
        levels = over->levels;
        pages = drawn.size();
    }
    CHECK(pages > 1);
}
