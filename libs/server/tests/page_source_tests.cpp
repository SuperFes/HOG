#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/page_source.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using hog::grid::Key;
using hog::server::PageSource;

namespace {

Key ground_key(const hog::globe::Generator& generator, hog::DVec3 towards, std::uint8_t depth) {
    const hog::DVec3 direction = glm::normalize(towards);
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    return hog::globe::grid_of(generator).key_at(place, depth).value();
}

} // namespace

TEST_CASE("pages held are served from the store alone, and those missing filled when asked",
          "[server][page_source]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{3};
    const Key finest = ground_key(generator, {0.2, 0.7, -0.68}, grid.depth());
    {
        hog::baked::Filler filler{generator, *store, jobs};
        filler.want(finest);
        while (!filler.idle()) {
            REQUIRE(filler.pump(true).has_value());
        }
    }

    // No filler and so no generator: what's held is all there is.
    {
        auto source = PageSource::open(*store);
        REQUIRE(source.has_value());
        const Key held = grid.page_of(finest);
        const auto page = source->page(held);
        REQUIRE(page.has_value());
        const hog::globe::Filled filled =
            hog::globe::fill(generator, grid, held, grid.levels_under(held.depth));
        CHECK(page.value().shape.value().tree.same_as(filled.shape));
        CHECK(page.value().material.value().tree.same_as(filled.material));
        CHECK(page.value().water.value().tree.same_as(filled.water));
        const Key elsewhere = ground_key(generator, {-0.6, 0.1, 0.79}, 4);
        CHECK_FALSE(source->page(elsewhere).has_value());
        const auto pumped = source->pump();
        REQUIRE(pumped.has_value());
        CHECK(pumped->empty());
        CHECK_FALSE(source->page(elsewhere).has_value());
    }

    // With a filler, a page missing is filled once asked for, and read on the next pump.
    hog::baked::Filler filler{generator, *store, jobs};
    auto source = PageSource::open(*store, &filler);
    REQUIRE(source.has_value());
    const Key elsewhere = ground_key(generator, {-0.6, 0.1, 0.79}, 4);
    CHECK_FALSE(source->page(elsewhere).has_value());
    std::vector<Key> written;
    while (!filler.idle()) {
        const auto pumped = source->pump(true);
        REQUIRE(pumped.has_value());
        written.insert(written.end(), pumped->begin(), pumped->end());
    }
    CHECK(written == std::vector<Key>{elsewhere});
    CHECK(source->page(elsewhere).has_value());
}
