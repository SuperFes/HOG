#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/page_sending.hpp>
#include <hog/server/page_source.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <vector>

using hog::grid::Key;
using hog::server::PageSending;
using hog::server::PageSource;

TEST_CASE("pages are sent to a player standing still coarse first, then nearest first",
          "[server][page_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    const hog::DVec3 up = glm::normalize(hog::DVec3{0.5, -0.3, 0.81});
    const hog::DVec3 at =
        up * (generator.column(up, hog::globe::voxel_lod(generator)).ground() + 1.7);
    const Key finest = grid.key_at(at, grid.depth()).value();
    // Only the pages over the player are held.
    std::vector<Key> chain;
    for (Key page = grid.page_of(finest);; page = grid.page_of(page)) {
        chain.insert(chain.begin(), page);
        if (page.depth == 0) {
            break;
        }
    }
    {
        hog::jobs::JobSystem jobs{3};
        hog::baked::Filler filler{generator, *store, jobs};
        filler.want(finest);
        while (!filler.idle()) {
            REQUIRE(filler.pump(true).has_value());
        }
    }
    auto source = PageSource::open(*store);
    REQUIRE(source.has_value());

    PageSending sending{grid};
    const std::vector<PageSending::Want> wanted = sending.wanted(at, *source);
    REQUIRE_FALSE(wanted.empty());
    CHECK(wanted.front().page == Key{});
    CHECK(std::ranges::is_sorted(wanted, [&](const auto& a, const auto& b) {
        return std::tuple{a.page.depth, grid.distance(a.page, at)} <
               std::tuple{b.page.depth, grid.distance(b.page, at)};
    }));
    // Those over the player go whole.
    for (const Key page : chain) {
        CHECK(std::ranges::find(wanted, PageSending::Want{.page = page,
                                                          .levels = grid.levels_under(
                                                              page.depth)}) != wanted.end());
    }
    // Each finer page wanted lies near enough for the node it refines to span more than the
    // pixels allowed, and goes as deep as its nodes should be seen from there.
    const double angle =
        PageSending::node_pixels * PageSending::view_fov / PageSending::view_height;
    for (const auto& want : wanted) {
        CAPTURE(want.page.depth);
        const double away = grid.distance(want.page, at);
        CHECK(away * angle < grid.size(want.page.depth));
        CHECK(want.levels == sending.levels_at(want.page, away));
    }
    const Key fine = chain.back();
    CHECK(sending.levels_at(fine, 0.0) == grid.levels_under(fine.depth));
    CHECK(sending.levels_at(fine, grid.size(fine.depth) / angle) == 1);
    CHECK(std::ranges::count_if(wanted, [&](const auto& want) {
              return want.page.depth == fine.depth && want.levels < grid.levels_under(fine.depth);
          }) > 1);

    // Sent in that order as far as they're held, a page at a time when there's room for one.
    const hog::net::ConnectionId player{7};
    sending.join(player);
    CHECK(sending.moved(player, at));
    sending.follow(player, at, *source);
    CHECK_FALSE(sending.moved(player, at));
    CHECK(sending.due(player) == wanted.size());
    std::vector<Key> sent;
    for (std::size_t round = 0; round < chain.size() + 1; ++round) {
        const auto pages = sending.next(player, 1, *source);
        CHECK(pages.size() <= 1);
        for (const auto& page : pages) {
            CHECK_FALSE(page.bytes.empty());
            sent.push_back(page.root);
        }
    }
    CHECK(sent == chain);
    CHECK(sending.due(player) == wanted.size() - chain.size());

    // Standing still, nothing is sent again.
    sending.follow(player, at, *source);
    CHECK(sending.due(player) == wanted.size() - chain.size());
    CHECK(sending.next(player, std::size_t{1} << 30U, *source).empty());
}
