#include <hog/bake/bake.hpp>
#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using hog::bake::depth_of;
using hog::bake::Reach;
using hog::bake::wanted;
using hog::baked::Bands;
using hog::baked::Region;
using hog::grid::Key;

namespace {

Reach coarse(double metres) {
    return {.coarse = metres, .around = std::nullopt, .across = 0.0};
}

} // namespace

TEST_CASE("a bake goes down through the band holding the level its size rounds to", "[bake]") {
    // 1 m nodes 11 deep.
    const hog::grid::Grid grid{1000.0, 1.0};
    CHECK(depth_of(grid, std::numeric_limits<double>::infinity()) == 0);
    CHECK(depth_of(grid, 2048.0) == 0);
    CHECK(depth_of(grid, 1000.0) == 2);
    CHECK(depth_of(grid, 2.0) == 10);
    CHECK(depth_of(grid, 1.5) == 11);
    CHECK(depth_of(grid, 0.1) == 11);

    // Whatever the finest size, the same size asked gives nodes no wider, the coarser level's
    // wider.
    for (const hog::grid::Grid each : {hog::grid::Grid{1000.0, 1.0}, hog::grid::Grid{1000.0, 0.3},
                                       hog::grid::Grid{30000.0, 0.7}, hog::grid::Grid{5.0, 0.01}}) {
        for (const double metres : {0.5, 3.0, 40.0, 700.0}) {
            CAPTURE(each.depth(), each.finest(), metres);
            const std::uint8_t depth = depth_of(each, metres);
            CHECK((each.size(depth) <= metres || depth == each.depth()));
            CHECK((depth == 0 || each.size(static_cast<std::uint8_t>(depth - 1)) > metres));
        }
    }

    // Bands of 3: level 5 lies in band 1, down to level 6.
    const Bands bands{3, grid.depth()};
    const Key node{.depth = 3, .x = 2, .y = 5, .z = 1};
    CHECK(wanted(grid, bands, coarse(grid.size(5)), {.band = 1, .root = node}, true));
    CHECK_FALSE(wanted(grid, bands, coarse(grid.size(5)), {.band = 2, .root = node}, true));
    CHECK_FALSE(wanted(grid, bands, coarse(grid.size(5)), {.band = 1, .root = node}, false));
    CHECK_FALSE(wanted(grid, bands, Reach{}, {.band = 0, .root = Key{}}, true));
    // Near the place, any band, caves too.
    const Reach near{.around = grid.centre(node), .across = 1.0};
    CHECK(wanted(grid, bands, near, {.band = 3, .root = node}, false));
    CHECK_FALSE(
        wanted(grid, bands, near, {.band = 3, .root = node.beside(2, 0, 0).value()}, false));
}

TEST_CASE("a bake holds what the generator fills, once, and deepens without filling again",
          "[bake]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    auto store = hog::baked::ChunkStore::scratch(hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    const Bands& bands = store->bands();
    hog::jobs::JobSystem jobs{2};
    const hog::bake::Pace pace{.batch = 2, .fill_levels = 6};
    const hog::baked::Regions regions{bands, pace.fill_levels};

    // Level 4 lies in band 1; regions of both are the root's.
    const Reach shallow = coarse(grid.size(4));
    const auto first = hog::bake::bake(generator, *store, jobs, shallow, pace);
    REQUIRE(first.has_value());
    CHECK(first->filled == 2);
    CHECK(first->kept == 0);

    const hog::DVec3 direction = glm::normalize(hog::DVec3{0.3, -0.2, 0.93});
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    const Reach near{.around = place, .across = 0.5};
    std::vector<std::size_t> reported;
    const auto deeper =
        hog::bake::bake(generator, *store, jobs, near, pace,
                        [&](std::size_t done, std::size_t) { reported.push_back(done); });
    REQUIRE(deeper.has_value());
    CHECK(deeper->kept == 2);
    CHECK(deeper->filled >= bands.count() - 2U);
    CHECK(deeper->shape_bytes > 0);
    REQUIRE_FALSE(reported.empty());
    CHECK(reported.back() == deeper->filled + deeper->kept);

    // Every band's region over the place, as a fill of it gives.
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    for (std::uint8_t band = 0; band < bands.count(); ++band) {
        const Region region = regions.over(band, grid.key_at(place, bands.foot(band)).value());
        CAPTURE(band, region.root.depth);
        REQUIRE(hog::baked::holds(*snapshot, region));
        const hog::globe::Filled filled = hog::baked::fill_region(generator, grid, bands, region);
        const auto shape = snapshot->piece<hog::grid::Shape>(region.root, bands.foot(band));
        const auto water = snapshot->piece<hog::grid::Water>(region.root, bands.foot(band));
        REQUIRE(shape.has_value());
        REQUIRE(water.has_value());
        CHECK(shape.value().tree.same_as(filled.shape));
        CHECK(water.value().tree.same_as(filled.water));
    }

    const auto again = hog::bake::bake(generator, *store, jobs, near, pace);
    REQUIRE(again.has_value());
    CHECK(again->filled == 0);
    CHECK(again->kept == deeper->filled + deeper->kept);
}
