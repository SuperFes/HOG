#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/baked/generating.hpp>
#include <hog/baked/wants_table.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using hog::baked::ChunkStore;
using hog::baked::Generating;
using hog::baked::Logged;
using hog::baked::WantsTable;
using hog::grid::Key;

namespace {

ChunkStore scratch(const hog::globe::Generator& generator) {
    auto store =
        ChunkStore::scratch(hog::baked::baking_of(generator), {.map_bytes = std::uint64_t{1} << 30U,
                                                               .readers = 64,
                                                               .read_only = false,
                                                               .band_height = 3,
                                                               .budget = std::size_t{16} << 10U});
    REQUIRE(store.has_value());
    return std::move(*store);
}

Key ground_key(const hog::globe::Generator& generator, hog::DVec3 towards, std::uint8_t depth) {
    const hog::DVec3 direction = glm::normalize(towards);
    const hog::DVec3 place =
        direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    return hog::globe::grid_of(generator).key_at(place, depth).value();
}

// Steps until nothing's being filled, and once more to take in the table as it is.
void settle(Generating& generating) {
    for (bool busy = true; busy;) {
        const auto stepped = generating.step(true);
        REQUIRE(stepped.has_value());
        busy = *stepped;
    }
    REQUIRE(generating.step(false).has_value());
}

std::vector<Logged> answered(const ChunkStore& store) {
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    std::vector<Logged> found;
    for (const Logged& logged : snapshot->logged_since(0, 1U << 20U)) {
        if (logged.what == Logged::What::Answered) {
            found.push_back(logged);
        }
    }
    return found;
}

} // namespace

TEST_CASE("what the wants table asks for is filled into the store and logged as answered",
          "[baked][generating]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    ChunkStore store = scratch(generator);
    const hog::testing::TemporaryDirectory directory;
    auto wants = WantsTable::open(directory.path());
    REQUIRE(wants.has_value());
    hog::jobs::JobSystem jobs{2};
    Generating generating{generator, store, *wants, jobs, {.fill_levels = 3, .layered = {}}};

    const Key node = ground_key(generator, {0.2, 0.7, -0.68}, 9);
    const std::vector<WantsTable::Want> wanted{{.node = node, .depth = 12, .rank = {}}};
    REQUIRE(wants->replace(wanted).has_value());
    settle(generating);

    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const auto piece = snapshot->piece<hog::grid::Shape>(node, 12);
    REQUIRE(piece.has_value());
    CHECK(piece.value().tree.levels() == 3);
    const std::vector<Logged> told = answered(store);
    REQUIRE(told.size() == 1);
    CHECK(told.front().node == node);
    CHECK(told.front().depth == 12);
    // Chunks were logged before it was.
    CHECK(snapshot->first_logged() < told.front().at);

    // Still wanted, it isn't answered again.
    REQUIRE(generating.step(false).has_value());
    CHECK(answered(store).size() == 1);
}

TEST_CASE("a want taken out of the table is never answered", "[baked][generating]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    ChunkStore store = scratch(generator);
    const hog::testing::TemporaryDirectory directory;
    auto wants = WantsTable::open(directory.path());
    REQUIRE(wants.has_value());
    hog::jobs::JobSystem jobs{1};
    Generating generating{generator, store, *wants, jobs, {.fill_levels = 3, .layered = {}}};

    const Key kept = ground_key(generator, {0.2, 0.7, -0.68}, 9);
    const Key dropped = ground_key(generator, {-0.6, 0.1, 0.79}, 9);
    std::vector<WantsTable::Want> wanted{
        {.node = kept, .depth = 12, .rank = {.round = 1, .priority = 1.0}},
        {.node = dropped, .depth = 12, .rank = {.round = 1, .priority = 0.5}}};
    REQUIRE(wants->replace(wanted).has_value());
    REQUIRE(generating.step(false).has_value());
    wanted.pop_back();
    REQUIRE(wants->replace(wanted).has_value());
    settle(generating);
    const std::vector<Logged> told = answered(store);
    REQUIRE(told.size() == 1);
    CHECK(told.front().node == kept);
}
