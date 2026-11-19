#include <hog/bake/bake.hpp>
#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using hog::bake::depth_of;
using hog::bake::levels_for;
using hog::bake::pages_under;
using hog::bake::Reach;
using hog::grid::Key;

namespace {

Reach coarse(double metres) {
    return {.coarse = metres, .around = std::nullopt, .across = 0.0};
}

} // namespace

TEST_CASE("pages are wanted under open leaves where coarse enough, or near the place", "[bake]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    const Key top{};
    const Key foot{.depth = 5, .x = 9, .y = 20, .z = 3};
    // Pages under the top one start at 64 m nodes, so anything finer wants them.
    CHECK(pages_under(grid, coarse(1.0), top, foot, true) == std::vector<Key>{foot});
    CHECK(pages_under(grid, coarse(32.0), top, foot, true) == std::vector<Key>{foot});
    CHECK(pages_under(grid, coarse(64.0), top, foot, true).empty());
    CHECK(pages_under(grid, coarse(1.0), top, foot, false).empty());
    // Near the place, caves too.
    const hog::DVec3 inside = grid.centre(foot);
    const Reach near{.around = inside, .across = 1.0};
    CHECK(pages_under(grid, near, top, foot, false) == std::vector<Key>{foot});
    CHECK(pages_under(grid, near, top, foot.beside(2, 0, 0).value(), false).empty());
    // A leaf over the top page's feet leads to the pages at them under it.
    const Key shallow = foot.ancestor(3);
    CHECK(pages_under(grid, coarse(1.0), top, shallow, true).size() == 64);
    CHECK(pages_under(grid, near, top, shallow, false) == std::vector<Key>{foot});
    // A page that's one open leaf leads to the pages at its foot, never back to itself.
    CHECK(pages_under(grid, near, top, top, false) == std::vector<Key>{foot});
    // None under the finest nodes.
    CHECK(pages_under(grid, coarse(0.0), foot, foot, true).empty());
}

TEST_CASE("a bake goes down to the level its size rounds to, wherever pages are cut", "[bake]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    CHECK(depth_of(grid, std::numeric_limits<double>::infinity()) == 0);
    CHECK(depth_of(grid, 2048.0) == 0);
    CHECK(depth_of(grid, 1000.0) == 2);
    CHECK(depth_of(grid, 2.0) == 10);
    CHECK(depth_of(grid, 1.5) == 11);
    CHECK(depth_of(grid, 0.1) == 11);

    CHECK(levels_for(grid, coarse(1000.0), Key{}, true) == 2);
    CHECK(levels_for(grid, coarse(1.0), Key{}, true) == 5);
    const Key page{.depth = 5, .x = 9, .y = 20, .z = 3};
    CHECK(levels_for(grid, coarse(64.0), page, true) == 0);
    CHECK(levels_for(grid, coarse(32.0), page, true) == 1);
    CHECK(levels_for(grid, coarse(2.0), page, true) == 5);
    CHECK(levels_for(grid, coarse(0.5), page, true) == 6);
    CHECK(levels_for(grid, coarse(1.0), page, false) == 0);
    const Reach near{.around = grid.centre(page), .across = 1.0};
    CHECK(levels_for(grid, near, page, false) == 6);

    // Whatever the finest size and wherever pages are cut, the same size asked gives nodes no
    // wider, and the coarser level's wider, with every page filled down to it.
    for (const hog::grid::Grid each : {hog::grid::Grid{1000.0, 1.0}, hog::grid::Grid{1000.0, 0.3},
                                       hog::grid::Grid{30000.0, 0.7}, hog::grid::Grid{5.0, 0.01}}) {
        for (const double metres : {0.5, 3.0, 40.0, 700.0}) {
            CAPTURE(each.depth(), each.finest(), metres);
            const std::uint8_t depth = depth_of(each, metres);
            CHECK((each.size(depth) <= metres || depth == each.depth()));
            CHECK((depth == 0 || each.size(static_cast<std::uint8_t>(depth - 1)) > metres));
            for (std::uint8_t root = 0; root < each.depth();
                 root = static_cast<std::uint8_t>(root + each.levels_under(root))) {
                const std::uint8_t levels =
                    levels_for(each, coarse(metres), Key{.depth = root}, true);
                const int foot = root + each.levels_under(root);
                CHECK(root + levels == (depth <= root ? root : std::min<int>(depth, foot)));
            }
        }
    }
}

TEST_CASE("a bake fills pages as deep as asked, deepening those held shallower", "[bake]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{2};
    const std::uint8_t whole = grid.levels_under(0);
    REQUIRE(whole > 1);
    const auto top_levels = [&] {
        auto snapshot = store->read();
        REQUIRE(snapshot.has_value());
        const auto read = snapshot->page(Key{});
        REQUIRE(read.has_value());
        CHECK(read.value().page.shape.value().tree.same_as(
            hog::globe::fill(generator, grid, Key{}, read.value().page.shape->tree.levels())
                .shape));
        return read.value().page.shape->tree.levels();
    };

    // Short of the top page's foot, only the top page, only that deep.
    const Reach shallow = coarse(grid.size(static_cast<std::uint8_t>(whole - 1)));
    const auto first = hog::bake::bake(generator, *store, jobs, shallow);
    REQUIRE(first.has_value());
    CHECK(first->filled == 1);
    CHECK(top_levels() == whole - 1);

    const Reach deep = coarse(grid.size(whole));
    const auto deeper = hog::bake::bake(generator, *store, jobs, deep);
    REQUIRE(deeper.has_value());
    CHECK(deeper->filled == 1);
    CHECK(deeper->kept == 0);
    CHECK(top_levels() == whole);

    const auto again = hog::bake::bake(generator, *store, jobs, shallow);
    REQUIRE(again.has_value());
    CHECK(again->filled == 0);
    CHECK(again->kept == 1);
}

TEST_CASE("a bake stores the pages down to a place as the generator fills them, once", "[bake]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{2};
    const hog::DVec3 direction = glm::normalize(hog::DVec3{0.3, -0.2, 0.93});
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    const Reach reach{.around = place, .across = 0.5};
    std::vector<std::size_t> reported;
    const auto baked =
        hog::bake::bake(generator, *store, jobs, reach, 2,
                        [&](std::size_t done, std::size_t) { reported.push_back(done); });
    REQUIRE(baked.has_value());
    CHECK(baked->kept == 0);
    CHECK(baked->shape_bytes > 0);
    REQUIRE_FALSE(reported.empty());
    CHECK(reported.back() == baked->filled);

    // Every page down to the finest over the place, each as a fill of it gives.
    const Key finest = grid.key_at(place, grid.depth()).value();
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->holder<hog::grid::Shape>(finest) == grid.page_of(finest));
    std::size_t pages = 0;
    for (Key page = grid.page_of(finest);; page = grid.page_of(page)) {
        CAPTURE(page.depth);
        const auto read = snapshot->page(page);
        REQUIRE(read.has_value());
        const hog::globe::Filled filled =
            hog::globe::fill(generator, grid, page, grid.levels_under(page.depth));
        CHECK(read.value().page.shape.value().tree.same_as(filled.shape));
        CHECK(read.value().page.material.value().tree.same_as(filled.material));
        CHECK(read.value().page.water.value().tree.same_as(filled.water));
        ++pages;
        if (page.depth == 0) {
            break;
        }
    }
    CHECK(baked->filled >= pages);

    const auto again = hog::bake::bake(generator, *store, jobs, reach);
    REQUIRE(again.has_value());
    CHECK(again->filled == 0);
    CHECK(again->kept == baked->filled);
}
