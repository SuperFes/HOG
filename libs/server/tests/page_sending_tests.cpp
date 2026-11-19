#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/view.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/page_sending.hpp>
#include <hog/server/page_source.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

using hog::grid::Key;
using hog::server::PageSending;
using hog::server::PageSource;

TEST_CASE("pages are sent to a player standing still biggest looking first, the rest when filled",
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
    const auto place = [&](Key page) {
        return std::ranges::find(wanted, page, &PageSending::Want::page) - wanted.begin();
    };
    // By how big each looks, so near detail before far, and every page after the one over it.
    for (const auto& want : wanted) {
        CAPTURE(want.page.depth);
        CHECK(want.priority ==
              grid.size(want.page.depth) / (grid.distance(want.page, at) + grid.finest()));
        if (want.page.depth > 0) {
            CHECK(place(grid.page_of(want.page)) < place(want.page));
        }
    }
    CHECK(std::ranges::is_sorted(wanted, std::ranges::greater{}, &PageSending::Want::priority));
    // Those over the player go whole.
    for (const Key page : chain) {
        REQUIRE(place(page) < std::ssize(wanted));
        CHECK(wanted[static_cast<std::size_t>(place(page))].levels ==
              grid.levels_under(page.depth));
    }
    // Each finer page wanted lies near enough for the node it refines to span more than the
    // pixels allowed, and goes as deep as its nodes should be seen from there.
    const hog::grid::View view;
    const double angle = view.angle;
    for (const auto& want : wanted) {
        CAPTURE(want.page.depth);
        const double away = grid.distance(want.page, at);
        CHECK(away * angle < grid.size(want.page.depth));
        CHECK(want.levels == view.levels_at(grid, want.page, away));
    }
    const Key fine = chain.back();
    CHECK(view.levels_at(grid, fine, 0.0) == grid.levels_under(fine.depth));
    CHECK(view.levels_at(grid, fine, grid.size(fine.depth) / angle) == 1);
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
    // Those not held wait, asked for once, rather than being read again each time.
    CHECK(sending.due(player) == 0);
    CHECK(sending.waiting(player) == wanted.size() - chain.size());

    // Standing still, nothing is sent again.
    sending.follow(player, at, *source);
    CHECK(sending.due(player) == wanted.size() - chain.size());
    CHECK(sending.next(player, std::size_t{1} << 30U, *source).empty());
    CHECK(sending.waiting(player) == wanted.size() - chain.size());

    // One written is due again, and goes.
    const auto beside = std::ranges::find_if(wanted, [&](const auto& want) {
        return want.page.depth == chain.back().depth && want.page != chain.back();
    });
    REQUIRE(beside != wanted.end());
    const std::vector<Key> written{beside->page};
    const std::vector<std::optional<hog::globe::Filled>> filled{
        hog::globe::fill(generator, grid, beside->page, grid.levels_under(beside->page.depth))};
    REQUIRE(hog::baked::write_filled(*store, written, filled).has_value());
    REQUIRE(source->pump().has_value());
    sending.changed(written);
    CHECK(sending.due(player) == 1);
    CHECK(sending.waiting(player) == wanted.size() - chain.size() - 1);
    const auto more = sending.next(player, std::size_t{1} << 30U, *source);
    REQUIRE(more.size() == 1);
    CHECK(more.front().root == beside->page);
}

TEST_CASE("page sending cost for a player arriving on the small world", "[.bench]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    auto store = hog::baked::PageStore::scratch(hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{4};
    hog::baked::Filler filler{generator, *store, jobs};
    auto source = PageSource::open(*store, &filler);
    REQUIRE(source.has_value());
    const hog::DVec3 up = glm::normalize(hog::DVec3{0.5, -0.3, 0.81});
    const hog::DVec3 at =
        up * (generator.column(up, hog::globe::voxel_lod(generator)).ground() + 1.7);
    PageSending sending{grid};
    const hog::net::ConnectionId player{1};
    sending.join(player);
    const auto started = std::chrono::steady_clock::now();
    double following = 0.0;
    std::size_t follows = 0;
    std::size_t pages = 0;
    std::size_t bytes = 0;
    std::size_t filled = 0;
    for (;;) {
        const auto before = std::chrono::steady_clock::now();
        sending.follow(player, at, *source);
        following +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
        ++follows;
        for (auto sent = sending.next(player, std::size_t{1} << 30U, *source); !sent.empty();
             sent = sending.next(player, std::size_t{1} << 30U, *source)) {
            pages += sent.size();
            for (const auto& page : sent) {
                bytes += page.bytes.size();
            }
        }
        if (filler.idle()) {
            break;
        }
        while (!filler.idle()) {
            const auto written = source->pump(true);
            REQUIRE(written.has_value());
            filled += written->size();
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    WARN(pages << " pages sent, " << static_cast<double>(bytes) / 1e6 << " MB; " << filled
               << " filled; " << seconds << " s in all, " << follows << " follows taking "
               << following << " s; " << sending.due(player) << " still due");
}

TEST_CASE("a page whose water changes sends only its water tree and bodies not yet told",
          "[server][page_sending]") {
    // 1 m nodes 11 deep: a top page of 5 levels, here all solid, so the only page wanted.
    const hog::grid::Grid grid{1000.0, 1.0};
    auto store = hog::baked::PageStore::scratch({.world = 42, .grid = grid, .generator = 1});
    REQUIRE(store.has_value());
    const auto water = [](std::uint32_t body) {
        return hog::grid::Tree<hog::grid::Water>::build(
            5, [&](int x, int /*y*/, int /*z*/) { return x < 8 ? body : 0U; });
    };
    const auto put = [&](const auto& tree) {
        REQUIRE(store
                    ->write([&](hog::baked::PageStore::Batch& batch) -> hog::Result<void> {
                        const auto held = batch.put(Key{}, tree);
                        if (!held) {
                            return std::unexpected{held.error()};
                        }
                        return {};
                    })
                    .has_value());
    };
    put(hog::grid::Tree<hog::grid::Shape>::build(
        5, [](int, int, int) { return hog::grid::Surface::solid(); }));
    put(hog::grid::Tree<hog::grid::Material>::build(
        5, [](int, int, int) { return std::uint16_t{3}; }));
    put(water(1));
    auto source = PageSource::open(*store);
    REQUIRE(source.has_value());

    PageSending sending{grid};
    const hog::net::ConnectionId player{2};
    sending.join(player);
    const hog::DVec3 at{10.0, 0.0, 0.0};
    sending.follow(player, at, *source);
    const auto first = sending.next(player, std::size_t{1} << 20U, *source);
    REQUIRE(first.size() == 1);
    const auto whole = hog::grid::Page::decode(first.front().bytes);
    REQUIRE(whole.has_value());
    CHECK(whole->shape.has_value());
    CHECK(whole->material.has_value());
    CHECK(whole->water.has_value());
    CHECK(first.front().bodies == std::vector<hog::grid::Water::Value>{1});

    // Nothing changed, nothing goes.
    sending.changed(std::vector<Key>{Key{}});
    CHECK(sending.due(player) == 1);
    CHECK(sending.next(player, std::size_t{1} << 20U, *source).empty());
    CHECK(sending.due(player) == 0);

    put(water(2));
    REQUIRE(source->pump().has_value());
    sending.changed(std::vector<Key>{Key{}});
    const auto changed = sending.next(player, std::size_t{1} << 20U, *source);
    REQUIRE(changed.size() == 1);
    const auto page = hog::grid::Page::decode(changed.front().bytes);
    REQUIRE(page.has_value());
    CHECK_FALSE(page->shape.has_value());
    CHECK_FALSE(page->material.has_value());
    REQUIRE(page->water.has_value());
    CHECK(page->water.value().version == 2);
    CHECK(page->water.value().tree.same_as(water(2)));
    CHECK(changed.front().bodies == std::vector<hog::grid::Water::Value>{2});
    CHECK(sending.due(player) == 0);
    sending.follow(player, at, *source);
    CHECK(sending.next(player, std::size_t{1} << 20U, *source).empty());

    // A body told once isn't told again.
    put(water(1));
    REQUIRE(source->pump().has_value());
    sending.changed(std::vector<Key>{Key{}});
    const auto back = sending.next(player, std::size_t{1} << 20U, *source);
    REQUIRE(back.size() == 1);
    CHECK(back.front().bodies.empty());
}

TEST_CASE("a player asking is sent what it asks for in its order, and nothing else",
          "[server][page_sending]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    auto store = hog::baked::PageStore::scratch({.world = 42, .grid = grid, .generator = 1});
    REQUIRE(store.has_value());
    const auto put = [&](Key root, std::uint8_t levels) {
        REQUIRE(store
                    ->write([&](hog::baked::PageStore::Batch& batch) -> hog::Result<void> {
                        for (const auto& held :
                             {batch.put(root, hog::grid::Tree<hog::grid::Shape>::build(
                                                  levels,
                                                  [](int, int, int) {
                                                      return hog::grid::Surface::solid();
                                                  })),
                              batch.put(root, hog::grid::Tree<hog::grid::Material>::build(
                                                  levels,
                                                  [](int, int, int) { return std::uint16_t{3}; })),
                              batch.put(root, hog::grid::Tree<hog::grid::Water>::build(
                                                  levels, [](int, int, int) { return 0U; }))}) {
                            if (!held) {
                                return std::unexpected{held.error()};
                            }
                        }
                        return {};
                    })
                    .has_value());
    };
    const Key child{.depth = 5, .x = 1, .y = 2, .z = 3};
    const Key missing{.depth = 5, .x = 4, .y = 4, .z = 4};
    put(Key{}, 5);
    put(child, 6);
    auto source = PageSource::open(*store);
    REQUIRE(source.has_value());

    PageSending sending{grid};
    const hog::net::ConnectionId player{3};
    sending.join(player);
    CHECK_FALSE(sending.asking(player));
    // Past the page's foot, not a page's root, and outside the grid: cut, and left out.
    const std::vector<PageSending::Want> asked{
        {.page = child, .levels = 9, .priority = 2.0},
        {.page = Key{}, .levels = 5, .priority = 1.0},
        {.page = missing, .levels = 3, .priority = 0.5},
        {.page = Key{.depth = 3}, .levels = 2, .priority = 9.0},
        {.page = Key{.depth = 5, .x = 99}, .levels = 1, .priority = 1.0},
        {.page = Key{.depth = 40}, .levels = 1, .priority = 1.0}};
    sending.ask(player, asked);
    CHECK(sending.asking(player));
    CHECK(sending.due(player) == 3);
    const auto first = sending.next(player, std::size_t{1} << 30U, *source);
    REQUIRE(first.size() == 2);
    CHECK(first[0].root == child);
    CHECK(first[1].root == Key{});
    const auto deep = hog::grid::Page::decode(first[0].bytes);
    REQUIRE(deep.has_value());
    CHECK(deep->shape.value().tree.levels() == 6);
    CHECK(sending.waiting(player) == 1);

    // Asked again, in place of before: what's been sent as deep isn't due again.
    const std::vector<PageSending::Want> again{{.page = child, .levels = 6, .priority = 1.0},
                                               {.page = missing, .levels = 3, .priority = 1.0}};
    sending.ask(player, again);
    CHECK(sending.due(player) == 1);
    CHECK(sending.next(player, std::size_t{1} << 30U, *source).empty());
    CHECK(sending.waiting(player) == 1);

    // Once it's written, it goes.
    put(missing, 6);
    REQUIRE(source->pump().has_value());
    sending.changed(std::vector<Key>{missing});
    const auto last = sending.next(player, std::size_t{1} << 30U, *source);
    REQUIRE(last.size() == 1);
    CHECK(last.front().root == missing);
    const auto cut = hog::grid::Page::decode(last.front().bytes);
    REQUIRE(cut.has_value());
    CHECK(cut->shape.value().tree.levels() == 3);
}
