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

#include <cstdint>
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
        const auto page = source->page(held, grid.levels_under(held.depth));
        REQUIRE(page.has_value());
        const hog::globe::Filled filled =
            hog::globe::fill(generator, grid, held, grid.levels_under(held.depth));
        CHECK(page.value().shape.value().tree.same_as(filled.shape));
        CHECK(page.value().material.value().tree.same_as(filled.material));
        CHECK(page.value().water.value().tree.same_as(filled.water));
        const Key elsewhere = ground_key(generator, {-0.6, 0.1, 0.79}, 4);
        CHECK_FALSE(source->page(elsewhere, grid.levels_under(elsewhere.depth)).has_value());
        const auto pumped = source->pump();
        REQUIRE(pumped.has_value());
        CHECK(pumped->empty());
        CHECK_FALSE(source->page(elsewhere, grid.levels_under(elsewhere.depth)).has_value());
    }

    // With a filler, a page missing is filled once asked for, and read on the next pump.
    hog::baked::Filler filler{generator, *store, jobs};
    auto source = PageSource::open(*store, &filler);
    REQUIRE(source.has_value());
    const Key elsewhere = ground_key(generator, {-0.6, 0.1, 0.79}, 4);
    CHECK_FALSE(source->page(elsewhere, grid.levels_under(elsewhere.depth)).has_value());
    std::vector<Key> written;
    while (!filler.idle()) {
        const auto pumped = source->pump(true);
        REQUIRE(pumped.has_value());
        written.insert(written.end(), pumped->begin(), pumped->end());
    }
    CHECK(written == std::vector<Key>{elsewhere});
    CHECK(source->page(elsewhere, grid.levels_under(elsewhere.depth)).has_value());
}

TEST_CASE("answers are kept for whoever asks next, and let go once their page changes or for room",
          "[server][page_source]") {
    // 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
    const hog::grid::Grid grid{1000.0, 1.0};
    auto store = hog::baked::PageStore::scratch({.world = 42, .grid = grid, .generator = 1});
    REQUIRE(store.has_value());
    using Materials = hog::grid::Tree<hog::grid::Material>;
    const auto materials = [](std::uint8_t levels, std::uint16_t first) {
        return Materials::build(levels, [&](int x, int /*y*/, int /*z*/) {
            return static_cast<std::uint16_t>(first + (x / 4));
        });
    };
    const auto put = [&](Key root, std::uint8_t levels, std::uint16_t first, std::uint32_t body) {
        REQUIRE(store
                    ->write([&](hog::baked::PageStore::Batch& batch) -> hog::Result<void> {
                        for (const auto& held :
                             {batch.put(root,
                                        hog::grid::Tree<hog::grid::Shape>{
                                            levels, hog::grid::Surface::solid(), false}),
                              batch.put(root, materials(levels, first)),
                              batch.put(root, hog::grid::Tree<hog::grid::Water>::build(
                                                  levels, [&](int x, int, int) {
                                                      return x < 8 ? body : 0U;
                                                  }))}) {
                            if (!held) {
                                return std::unexpected{held.error()};
                            }
                        }
                        return {};
                    })
                    .has_value());
    };
    const Key child{.depth = 5, .x = 1, .y = 2, .z = 3};
    put(Key{}, 5, 1, 1);
    put(child, 6, 3, 1);
    auto source = PageSource::open(*store);
    REQUIRE(source.has_value());

    // Asked again, the same answer, made once.
    const auto whole = source->answer(child, 6);
    REQUIRE(whole != nullptr);
    CHECK(source->answer(child, 6) == whole);
    CHECK(source->kept().made == 1);
    CHECK(source->kept().found == 1);
    CHECK(whole->levels() == 6);
    CHECK(whole->bytes == whole->page.encode());
    CHECK(whole->bodies == std::vector<hog::grid::Water::Value>{1});
    // Cut shallower, an answer of its own; asked deeper than held, the deepest there is.
    const auto shallow = source->answer(child, 3);
    REQUIRE(shallow != nullptr);
    CHECK(shallow != whole);
    CHECK(shallow->levels() == 3);
    CHECK(shallow->page.material.value().tree.same_as(materials(6, 3).trimmed(3)));
    CHECK(source->answer(child, 9) == whole);
    CHECK(source->answer(Key{.depth = 5, .x = 7}, 6) == nullptr);

    // Another writer changes the page: the store as last read still answers alike, and once
    // moved on to it, the answer is made afresh, the old one still whole for whoever holds it.
    put(child, 6, 4, 2);
    CHECK(source->answer(child, 6) == whole);
    REQUIRE(source->pump().has_value());
    const auto changed = source->answer(child, 6);
    REQUIRE(changed != nullptr);
    CHECK(changed != whole);
    CHECK(changed->bodies == std::vector<hog::grid::Water::Value>{2});
    CHECK(changed->page.material.value().tree.same_as(materials(6, 4)));
    CHECK(whole->page.material.value().tree.same_as(materials(6, 3)));
    CHECK(source->kept().let_go == 1);

    // Room for about one page: the one asked for less lately goes.
    auto small = PageSource::open(*store, nullptr, whole->bytes.size() + 1);
    REQUIRE(small.has_value());
    REQUIRE(small->answer(Key{}, 5) != nullptr);
    REQUIRE(small->answer(child, 6) != nullptr);
    CHECK(small->kept().pages == 1);
    CHECK(small->kept().let_go == 1);
    CHECK(small->answer(child, 6) != nullptr);
    CHECK(small->kept().found == 1);
}
