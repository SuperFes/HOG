#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <vector>

using hog::baked::Filler;
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

TEST_CASE("a node asked for is filled down to the page holding it, once", "[baked][filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
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

TEST_CASE("a node under a leaf that isn't open is answered by the page over it",
          "[baked][filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{2};
    Filler filler{generator, *store, jobs};
    // A corner of the grid, far out past the sky.
    filler.want(Key{.depth = hog::globe::grid_of(generator).depth()});
    CHECK(settle(filler) == std::vector<Key>{Key{}});
}

TEST_CASE("pages another generator version made are filled again", "[baked][filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::testing::TemporaryDirectory directory;
    const Key corner{.depth = hog::globe::grid_of(generator).depth()};
    hog::jobs::JobSystem jobs{2};
    hog::baked::Baking before = hog::baked::baking_of(generator);
    before.generator = hog::globe::generator_version + 1;
    {
        auto store = hog::baked::PageStore::open(directory.path(), before);
        REQUIRE(store.has_value());
        Filler filler{generator, *store, jobs};
        filler.want(corner);
        REQUIRE(settle(filler) == std::vector<Key>{Key{}});
    }
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    {
        auto snapshot = store->read();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->page(Key{}).value().stale ==
              hog::baked::Trees{.shape = true, .material = true, .water = true, .woods = true});
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

TEST_CASE("a page is filled whole, and again whole where held shallower than asked",
          "[baked][filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{3};
    Filler filler{generator, *store, jobs};
    const hog::DVec3 direction = glm::normalize(hog::DVec3{0.7, 0.6, -0.39});
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    const Key finest = grid.key_at(place, grid.depth()).value();
    const Key page = grid.page_of(finest);
    const Key shallow = finest.ancestor(static_cast<std::uint8_t>(page.depth + 2));
    const std::uint8_t whole = grid.levels_under(page.depth);
    const auto held = [&] {
        auto snapshot = store->read();
        REQUIRE(snapshot.has_value());
        auto shape = snapshot->tree<hog::grid::Shape>(page);
        REQUIRE(shape.has_value());
        return std::move(shape).value();
    };

    filler.want(shallow);
    CHECK(std::ranges::count(settle(filler), page) == 1);
    CHECK(held().tree.levels() == whole);
    CHECK(held().tree.same_as(hog::globe::fill(generator, grid, page, whole).shape));
    filler.want(finest);
    CHECK(settle(filler).empty());

    // As an older fill left it.
    const std::vector<Key> pages{page};
    const std::vector<std::optional<hog::globe::Filled>> shallower{
        hog::globe::fill(generator, grid, page, 2)};
    REQUIRE(hog::baked::write_filled(*store, pages, shallower).has_value());
    filler.want(shallow);
    CHECK(settle(filler).empty());
    filler.want(finest);
    CHECK(settle(filler) == pages);
    CHECK(held().tree.levels() == whole);
}

TEST_CASE("fills start by rank, a later round first, a page taking its highest asked",
          "[baked][filling]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::PageStore::open(directory.path(), hog::baked::baking_of(generator));
    REQUIRE(store.has_value());
    // One worker, so two fills at a time.
    hog::jobs::JobSystem jobs{1};
    Filler filler{generator, *store, jobs};
    // Six finest pages along the ground, the pages over them held already.
    std::vector<Key> pages;
    const hog::DVec3 start = glm::normalize(hog::DVec3{-0.2, 0.9, 0.38});
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, start));
    const double radius = generator.column(start, hog::globe::voxel_lod(generator)).ground();
    const double page_size = grid.size(grid.page_of(Key{.depth = grid.depth()}).depth);
    for (int step = 0; pages.size() < 6; ++step) {
        const double along = step * 1.5 * page_size / radius;
        const hog::DVec3 direction = glm::normalize(start + (east * along));
        const hog::DVec3 place =
            direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
        const Key page = grid.page_of(grid.key_at(place, grid.depth()).value());
        if (std::ranges::find(pages, page) == pages.end()) {
            pages.push_back(page);
        }
    }
    for (const Key page : pages) {
        filler.want(page);
    }
    settle(filler);

    // Ranked by place, but two asked again in later rounds.
    const auto corner = [&](Key page) {
        const std::uint8_t levels = grid.levels_under(page.depth);
        return Key{.depth = static_cast<std::uint8_t>(page.depth + levels),
                   .x = page.x << levels,
                   .y = page.y << levels,
                   .z = page.z << levels};
    };
    for (std::size_t n = 0; n < pages.size(); ++n) {
        filler.want(corner(pages[n]), {.round = 1, .priority = static_cast<double>(n)});
    }
    filler.want(corner(pages[0]), {.round = 2, .priority = 0.0});
    filler.want(corner(pages[2]), {.round = 3, .priority = 0.0});
    // The first two start; the rest wait in rank order.
    REQUIRE(filler.pump().has_value());
    CHECK(filler.queued() == std::vector<Key>{pages[5], pages[4], pages[3], pages[1]});

    std::vector<Key> filled = settle(filler);
    std::vector<Key> sorted = pages;
    std::ranges::sort(sorted);
    CHECK(filled == sorted);
}
