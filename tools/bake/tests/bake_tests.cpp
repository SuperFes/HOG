#include <hog/bake/bake.hpp>
#include <hog/baked/store.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

using hog::globe::Generator;
using hog::globe::TileKey;

namespace {

const Generator& planet() {
    static const Generator generator{hog::globe::Planet{}};
    return generator;
}

constexpr std::uint8_t level = 3;

// A block two tiles of `level` across holding the ground under `direction`: small enough to
// bake in a test.
TileKey ground_block(const Generator& generator, hog::DVec3 direction) {
    const hog::globe::Layers& layers = generator.layers();
    const hog::globe::Cell cell = hog::globe::cell_under(direction, layers.level);
    const double ground = generator.column(direction, hog::globe::voxel_lod(generator)).ground();
    const unsigned shift = 6 + level + 1;
    return {.level = level + 1,
            .face = cell.face,
            .i = cell.i >> shift,
            .j = cell.j >> shift,
            .k = static_cast<std::uint32_t>(layers.layer_at(ground)) >> shift};
}

} // namespace

TEST_CASE("blocks about a place hold it, on its face, up the shell", "[bake]") {
    const Generator& generator = planet();
    const hog::DVec3 direction{0.3, -0.2, 0.93};
    const std::vector<TileKey> blocks = hog::bake::blocks_around(generator, direction, 10.0, level);
    REQUIRE_FALSE(blocks.empty());
    const hog::globe::Cell cell = hog::globe::cell_under(direction, generator.layers().level);
    bool holds = false;
    for (const TileKey block : blocks) {
        CHECK(block.face == cell.face);
        CHECK(block.level == level + hog::bake::block_above);
        holds = holds || hog::bake::node_of(block).contains(
                             hog::baked::Point{.face = cell.face, .x = cell.i, .y = cell.j});
    }
    CHECK(holds);
    const std::vector<TileKey> face = hog::bake::face_blocks(generator, cell.face, level);
    CHECK(face.size() > blocks.size());
}

TEST_CASE("a bake stores the voxels as the generator makes them, once", "[bake]") {
    const Generator& generator = planet();
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::Store::open(directory.path(), {.map_bytes = std::uint64_t{1} << 30U});
    REQUIRE(store.has_value());
    hog::jobs::JobSystem jobs{2};
    const hog::DVec3 direction = glm::normalize(hog::DVec3{0.3, -0.2, 0.93});
    TileKey above = ground_block(generator, direction);
    ++above.k;
    const std::vector<TileKey> blocks{ground_block(generator, direction), above};
    std::vector<std::size_t> reported;
    const auto baked =
        hog::bake::bake(generator, *store, jobs, blocks, level, 1,
                        [&](std::size_t done, std::size_t) { reported.push_back(done); });
    REQUIRE(baked.has_value());
    CHECK(baked->made == blocks.size());
    CHECK(baked->skipped == 0);
    CHECK(reported == std::vector<std::size_t>{1, 2});

    const auto again = hog::bake::bake(generator, *store, jobs, blocks, level);
    REQUIRE(again.has_value());
    CHECK(again->made == 0);
    CHECK(again->skipped == blocks.size());

    // Where a leaf is known at the bake's level, it's the generator's voxel there.
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    const TileKey block = blocks.front();
    const hog::baked::Node node = hog::bake::node_of(block);
    std::size_t compared = 0;
    std::map<TileKey, hog::globe::VoxelTree> made_tiles;
    for (std::uint64_t n = 0; n < 4000 && compared < 200; ++n) {
        const auto pick = [&](std::uint64_t axis) {
            return static_cast<std::uint32_t>(hog::hash_values(axis, n) % node.side());
        };
        const hog::baked::Point p{
            .face = node.face, .x = node.x + pick(1), .y = node.y + pick(2), .z = node.z + pick(3)};
        const auto held = snapshot->at(p);
        REQUIRE(held.has_value());
        const hog::baked::Leaf leaf = held.value_or(hog::baked::Leaf{});
        if (leaf.open != hog::globe::made_at(level)) {
            continue;
        }
        const unsigned tile = 6 + level;
        const TileKey key{
            .level = level, .face = p.face, .i = p.x >> tile, .j = p.y >> tile, .k = p.z >> tile};
        auto found = made_tiles.find(key);
        if (found == made_tiles.end()) {
            found = made_tiles.emplace(key, hog::globe::generate_tile(generator, key)).first;
        }
        const hog::globe::VoxelTree& made = found->second;
        const auto within = [&](std::uint32_t at) {
            return static_cast<int>((at & ((1U << tile) - 1U)) >> level);
        };
        REQUIRE(leaf.cube == made.at(within(p.x), within(p.y), within(p.z)));
        ++compared;
    }
    CHECK(compared > 0);
}

TEST_CASE("a bake refuses a store of another planet", "[bake]") {
    const hog::testing::TemporaryDirectory directory;
    auto store = hog::baked::Store::open(directory.path());
    REQUIRE(store.has_value());
    REQUIRE(store->write([](hog::baked::Batch& batch) {
        const std::vector<std::byte> other{std::byte{1}};
        return batch.set_header(other);
    }));
    hog::jobs::JobSystem jobs{1};
    CHECK_FALSE(hog::bake::bake(planet(), *store, jobs, {}, level));
}
