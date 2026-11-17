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
#include <optional>
#include <vector>

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
    // Feet of the pages under the top one are 1 m across.
    CHECK(pages_under(grid, coarse(1.0), top, foot, true) == std::vector<Key>{foot});
    CHECK(pages_under(grid, coarse(2.0), top, foot, true).empty());
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
