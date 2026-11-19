#include "sloped_ground.hpp"

#include <hog/client/held_pages.hpp>
#include <hog/client/page_asking.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

using hog::client::pages_to_ask;
using hog::client::PageWish;
using hog::grid::Key;

namespace {

// The top page, the slope's surface open at its foot where it runs, so finer pages come under it.
std::vector<std::byte> top_page(const hog::grid::Grid& grid) {
    using Shapes = hog::grid::Tree<hog::grid::Shape>;
    const std::uint8_t levels = grid.levels_under(0);
    auto tree = Shapes::grow(
        levels,
        [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
            std::uint32_t z) -> std::optional<Shapes::Leaf> {
            const double side = grid.size(depth);
            const double above =
                hog::sloped::above_ground(grid.centre(Key{.depth = depth, .x = x, .y = y, .z = z}));
            if (std::abs(above) > side) {
                return Shapes::Leaf{.value = above > 0.0 ? hog::grid::Surface::open()
                                                         : hog::grid::Surface::solid(),
                                    .open = false};
            }
            if (depth == levels) {
                return Shapes::Leaf{
                    .value = hog::grid::Surface::plane(hog::sloped::normal(), above / side, 1),
                    .open = true};
            }
            return std::nullopt;
        });
    return hog::grid::Page{
        .root = Key{},
        .generator = 1,
        .shape = hog::grid::Held<hog::grid::Shape>{.version = 1, .tree = std::move(tree)},
        .material = std::nullopt,
        .water = std::nullopt,
        .woods = std::nullopt}
        .encode();
}

} // namespace

TEST_CASE("a client asks for the pages it lacks to see from where it is, biggest looking first",
          "[client][page_asking]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const hog::grid::View view;
    hog::client::HeldPages held{grid};
    const hog::DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 2.0);

    // Holding nothing, only the top page, as deep as it should be seen.
    const std::vector<PageWish> first = pages_to_ask(held, eye, std::nullopt, view, 100);
    REQUIRE(first.size() == 1);
    CHECK(first.front().page == Key{});
    CHECK(first.front().levels == view.levels_at(grid, Key{}, grid.distance(Key{}, eye)));

    // Holding it whole, the pages under it the slope runs through near enough to want finer.
    REQUIRE(held.take(top_page(grid)).has_value());
    const std::vector<PageWish> under = pages_to_ask(held, eye, std::nullopt, view, 10'000);
    REQUIRE(under.size() > 1);
    CHECK(std::ranges::is_sorted(under, std::ranges::greater{}, &PageWish::priority));
    for (const PageWish& wish : under) {
        CAPTURE(wish.page.depth, wish.page.x, wish.page.y, wish.page.z);
        const double away = grid.distance(wish.page, eye);
        CHECK(wish.page.depth == grid.levels_under(0));
        CHECK(away * view.angle < grid.size(wish.page.depth));
        CHECK(wish.levels == view.levels_at(grid, wish.page, away));
        CHECK(wish.priority == grid.size(wish.page.depth) / (away + grid.finest()));
    }
    const auto asked_for = [](const std::vector<PageWish>& wishes, Key page) {
        return std::ranges::find(wishes, page, &PageWish::page) != wishes.end();
    };
    CHECK(asked_for(under, hog::sloped::page_root));
    CHECK(under.front().page == hog::sloped::page_root);

    // One held as deep as it should be seen isn't asked for again.
    REQUIRE(held.take(hog::sloped::sloped(grid, hog::sloped::page_root)->encode()).has_value());
    const std::vector<PageWish> rest = pages_to_ask(held, eye, std::nullopt, view, 10'000);
    CHECK(rest.size() == under.size() - 1);
    CHECK_FALSE(asked_for(rest, hog::sloped::page_root));

    // Looking away from them, those wholly behind the eye are wanted less.
    const hog::DVec3 facing = hog::sloped::normal();
    const std::vector<PageWish> looking = pages_to_ask(held, eye, facing, view, 10'000);
    REQUIRE(looking.size() == rest.size());
    std::size_t behind = 0;
    for (const PageWish& wish : looking) {
        const auto plain = std::ranges::find(rest, wish.page, &PageWish::page);
        REQUIRE(plain != rest.end());
        if (wish.priority < plain->priority) {
            CHECK(wish.priority == plain->priority * hog::client::behind_the_eye);
            ++behind;
        }
    }
    CHECK(behind > 0);
    CHECK(behind < looking.size());

    // No more than asked for, the biggest looking kept.
    const std::vector<PageWish> few = pages_to_ask(held, eye, std::nullopt, view, 3);
    REQUIRE(few.size() == 3);
    CHECK(std::ranges::equal(few, rest | std::views::take(3)));
}

TEST_CASE("wishes go over the wire as asked, no more than allowed", "[client][page_asking]") {
    const std::vector<PageWish> wishes(
        hog::protocol::most_asked + 5,
        PageWish{.page = Key{.depth = 5, .x = 1}, .levels = 3, .priority = 0.5});
    const hog::protocol::Ask ask = hog::client::ask_of(wishes);
    REQUIRE(ask.pages.size() == hog::protocol::most_asked);
    CHECK(ask.pages.front() ==
          hog::protocol::AskedPage{.depth = 5, .x = 1, .levels = 3, .priority = 0.5F});
}
