#include <hog/bake/bake.hpp>
#include <hog/bake/filler.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using hog::bake::Filler;
using hog::grid::Key;

namespace {

// Pumps until nothing is wanted or being filled, giving the pages written.
std::vector<Key> settle(Filler& filler) {
    std::vector<Key> written;
    while (!filler.idle()) {
        const auto pumped = filler.pump(true);
        REQUIRE(pumped.has_value());
        written.insert(written.end(), pumped->begin(), pumped->end());
    }
    std::ranges::sort(written);
    return written;
}

} // namespace

TEST_CASE("a node asked for is filled down to the page holding it, once", "[bake][filler]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::bake::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{3};
    Filler filler{generator, *store, jobs};
    const hog::DVec3 direction = glm::normalize(hog::DVec3{-0.4, 0.5, 0.77});
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    const Key finest = grid.key_at(place, grid.depth()).value();
    filler.want(finest);
    filler.want(finest);

    std::vector<Key> chain;
    for (Key page = grid.page_of(finest);; page = grid.page_of(page)) {
        chain.push_back(page);
        if (page.depth == 0) {
            break;
        }
    }
    std::ranges::sort(chain);
    CHECK(settle(filler) == chain);
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->holder<hog::grid::Shape>(finest) == grid.page_of(finest));
    const auto read = snapshot->page(grid.page_of(finest));
    REQUIRE(read.has_value());
    const hog::globe::Filled filled = hog::globe::fill(
        generator, grid, grid.page_of(finest), grid.levels_under(grid.page_of(finest).depth));
    CHECK(read.value().page.shape.value().tree.same_as(filled.shape));

    // Held now, so asking again fills nothing.
    filler.want(finest);
    filler.want(finest.parent());
    CHECK(settle(filler).empty());
}

TEST_CASE("a node under a leaf that isn't open is answered by the page over it", "[bake][filler]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::bake::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{2};
    Filler filler{generator, *store, jobs};
    // A corner of the grid, far out past the sky.
    filler.want(Key{.depth = hog::globe::grid_of(generator).depth()});
    CHECK(settle(filler) == std::vector<Key>{Key{}});
}

TEST_CASE("pages another generator version made are filled again", "[bake][filler]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::testing::TemporaryDirectory directory;
    const Key corner{.depth = hog::globe::grid_of(generator).depth()};
    hog::jobs::JobSystem jobs{2};
    hog::baked::Baking before = hog::bake::baking_of(generator);
    before.generator = hog::globe::generator_version + 1;
    {
        auto store = hog::baked::PageStore::open(directory.path(), before);
        REQUIRE(store.has_value());
        Filler filler{generator, *store, jobs};
        filler.want(corner);
        REQUIRE(settle(filler) == std::vector<Key>{Key{}});
    }
    auto store = hog::baked::PageStore::open(directory.path(), hog::bake::baking_of(generator));
    REQUIRE(store.has_value());
    {
        auto snapshot = store->read();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->page(Key{}).value().stale ==
              hog::baked::Trees{.shape = true, .material = true, .water = true});
    }
    Filler filler{generator, *store, jobs};
    filler.want(corner);
    CHECK(settle(filler) == std::vector<Key>{Key{}});
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    const auto read = snapshot->page(Key{});
    REQUIRE(read.has_value());
    CHECK(read.value().stale == hog::baked::Trees{});
    // The same bytes, so the same version, now stamped as this generator's.
    CHECK(read.value().page.shape.value().version == 1);
}
