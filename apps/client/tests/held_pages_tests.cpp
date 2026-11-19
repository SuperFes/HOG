#include <hog/client/held_pages.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/water_bodies.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using hog::client::HeldPages;
using hog::grid::Key;
using hog::grid::Material;
using hog::grid::Tree;
using hog::grid::Water;

namespace {

// 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
hog::grid::Grid test_grid() {
    return {1000.0, 1.0};
}

// Materials changing along x every two nodes, `levels` deep, from `first`.
Tree<Material> stripes(std::uint8_t levels, std::uint16_t first) {
    return Tree<Material>::build(levels, [&](int x, int /*y*/, int /*z*/) {
        return static_cast<std::uint16_t>(first + (x / 2));
    });
}

std::vector<std::byte> coded(Key root, std::optional<Tree<Material>> material,
                             std::optional<Tree<Water>> water = std::nullopt,
                             std::uint32_t version = 1) {
    hog::grid::Page page{
        .root = root, .generator = 1, .shape = {}, .material = {}, .water = {}, .woods = {}};
    if (material) {
        page.material = hog::grid::Held<Material>{.version = version, .tree = *material};
    }
    if (water) {
        page.water = hog::grid::Held<Water>{.version = version, .tree = *water};
    }
    return page.encode();
}

} // namespace

TEST_CASE("held pages answer from the finest page over a node, as deep as it's held",
          "[client][held_pages]") {
    const hog::grid::Grid grid = test_grid();
    HeldPages held{grid};
    REQUIRE(held.take(coded(Key{}, stripes(5, 100))).has_value());
    const Key page{.depth = 5, .x = 3, .y = 4, .z = 5};
    REQUIRE(held.take(coded(page, stripes(2, 200))).has_value());
    CHECK(held.pages() == 2);

    // Under the page: what it holds, two levels down at most, where the stripes, alike in pairs,
    // merge a level up.
    const Key fine{.depth = 11, .x = (3U << 6U) + 9, .y = 4U << 6U, .z = 5U << 6U};
    const auto known = held.at<Material>(fine);
    REQUIRE(known.has_value());
    CHECK(known.value().page == page);
    CHECK(known.value().found.depth == 6);
    CHECK(known.value().found.value == 200);
    // Elsewhere: the top page's.
    const Key away{.depth = 11, .x = 9, .y = 9, .z = 9};
    const auto top = held.at<Material>(away);
    REQUIRE(top.has_value());
    CHECK(top.value().page == Key{});
    CHECK(top.value().found.depth == 4);
    // The page's own root: the page under it answers.
    CHECK(held.at<Material>(page).value().page == page);
    // A kind no page holds: nothing.
    CHECK_FALSE(held.at<Water>(fine).has_value());

    // Deeper, it's held in place of what was.
    REQUIRE(held.take(coded(page, stripes(6, 300))).has_value());
    CHECK(held.pages() == 2);
    const auto deeper = held.at<Material>(fine);
    REQUIRE(deeper.has_value());
    CHECK(deeper.value().found.depth == 10);
    CHECK(deeper.value().found.value == 300 + (9 / 2));
}

TEST_CASE("a snapshot of held pages holds them as they were", "[client][held_pages]") {
    const hog::grid::Grid grid = test_grid();
    HeldPages held{grid};
    REQUIRE(held.take(coded(Key{}, stripes(5, 100))).has_value());
    const Key page{.depth = 5, .x = 3, .y = 4, .z = 5};
    REQUIRE(held.take(coded(page, stripes(2, 200))).has_value());
    const hog::client::PagesSnapshot snapshot{grid, held.entries()};
    CHECK(snapshot.page(page) == held.page(page));
    CHECK(snapshot.shared(Key{}) == held.shared(Key{}));
    CHECK(snapshot.page(Key{.depth = 5, .x = 1, .y = 1, .z = 1}) == nullptr);
    // A page taken in anew is another; the snapshot keeps the one it had.
    REQUIRE(held.take(coded(page, stripes(2, 300))).has_value());
    CHECK(snapshot.page(page) != held.page(page));
    CHECK(snapshot.page(page) != nullptr);
}

TEST_CASE("a page sent with only the trees that changed keeps the rest", "[client][held_pages]") {
    HeldPages held{test_grid()};
    const auto water = [](std::uint32_t body) {
        return Tree<Water>::build(5, [&](int, int, int) { return body; });
    };
    REQUIRE(held.take(coded(Key{}, stripes(5, 1), water(1))).has_value());
    const std::size_t before = held.bytes();
    REQUIRE(held.take(coded(Key{}, std::nullopt, water(2), 2)).has_value());
    const hog::grid::Page* page = held.page(Key{});
    REQUIRE(page != nullptr);
    REQUIRE(page->material.has_value());
    CHECK(page->material.value().tree.same_as(stripes(5, 1)));
    REQUIRE(page->water.has_value());
    CHECK(page->water.value().version == 2);
    CHECK(held.at<Water>(Key{.depth = 5}).value().found.value == 2);
    CHECK(held.bytes() == before);
}

TEST_CASE("held pages refuse what isn't a page of the grid", "[client][held_pages]") {
    HeldPages held{test_grid()};
    CHECK_FALSE(held.take(std::vector<std::byte>{std::byte{1}, std::byte{2}}).has_value());
    // Not a page's root, and too deep for the page it's at.
    CHECK_FALSE(held.take(coded(Key{.depth = 3}, stripes(2, 1))).has_value());
    CHECK_FALSE(held.take(coded(Key{}, stripes(6, 1))).has_value());
    CHECK(held.pages() == 0);
}

TEST_CASE("held pages let go of the farthest first, never the top", "[client][held_pages]") {
    const hog::grid::Grid grid = test_grid();
    HeldPages held{grid};
    REQUIRE(held.take(coded(Key{}, stripes(5, 1))).has_value());
    const Key near{.depth = 5, .x = 16, .y = 16, .z = 16};
    const Key middle{.depth = 5, .x = 20, .y = 16, .z = 16};
    const Key far{.depth = 5, .x = 30, .y = 16, .z = 16};
    for (const Key page : {near, middle, far}) {
        REQUIRE(held.take(coded(page, stripes(6, 7))).has_value());
    }
    const hog::DVec3 eye = grid.centre(near);
    CHECK(held.evict(eye, held.bytes()).empty());
    CHECK(held.evict(eye, held.bytes() - 1) == std::vector<Key>{far});
    CHECK(held.page(far) == nullptr);
    CHECK(held.evict(eye, 0) == std::vector<Key>{middle, near});
    CHECK(held.pages() == 1);
    CHECK(held.page(Key{}) != nullptr);
}

TEST_CASE("held bodies are replaced by id", "[client][held_pages]") {
    HeldPages held{test_grid()};
    hog::grid::WaterBodies first;
    first.bodies.emplace(1U, hog::grid::Sea{.level = 10.0});
    first.bodies.emplace(2U, hog::grid::Lake{.level = 20.0, .season = {}});
    REQUIRE(held.take_bodies(first.encode()).has_value());
    hog::grid::WaterBodies second;
    second.bodies.emplace(2U, hog::grid::Lake{.level = 25.0, .season = {}});
    REQUIRE(held.take_bodies(second.encode()).has_value());
    REQUIRE(held.bodies().bodies.size() == 2);
    CHECK(std::get<hog::grid::Lake>(held.bodies().bodies.at(2U)).level == 25.0);
    CHECK(std::get<hog::grid::Sea>(held.bodies().bodies.at(1U)).level == 10.0);
    CHECK_FALSE(held.take_bodies(std::vector<std::byte>{std::byte{9}}).has_value());
}
