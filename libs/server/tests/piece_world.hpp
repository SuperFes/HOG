#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace hog::testing {

// The node `depth` deep on the ground towards `towards`.
inline grid::Key ground_key(const globe::Generator& generator, DVec3 towards, std::uint8_t depth) {
    const DVec3 direction = glm::normalize(towards);
    const DVec3 place =
        direction * generator.column(direction, globe::voxel_lod(generator)).ground();
    return globe::grid_of(generator).key_at(place, depth).value();
}

// A chunk store of its own, three levels a band.
inline baked::ChunkStore chunk_scratch(const globe::Generator& generator) {
    auto store = baked::ChunkStore::scratch(baked::baking_of(generator),
                                            {.map_bytes = std::uint64_t{1} << 30U,
                                             .readers = 64,
                                             .read_only = false,
                                             .band_height = 3,
                                             .budget = std::size_t{16} << 10U});
    REQUIRE(store.has_value());
    return std::move(*store);
}

// Has the store hold `node` down to `depth`.
inline void fill_chunks(const globe::Generator& generator, baked::ChunkStore& store, grid::Key node,
                        std::uint8_t depth) {
    jobs::JobSystem jobs{2};
    baked::ChunkFiller filler{generator, store, jobs, {.fill_levels = 3, .layered = {}}};
    filler.want(node, depth);
    while (!filler.idle()) {
        REQUIRE(filler.pump(true).has_value());
    }
}

// Has the store hold every node of the material band under `node` as `material`, the band
// rooted no deeper than the node.
inline void put_material(baked::ChunkStore& store, std::uint8_t band, grid::Key node,
                         std::uint16_t material) {
    const auto levels = static_cast<std::uint8_t>(store.bands().foot(band) - node.depth);
    const auto written = store.write([&](baked::ChunkStore::Batch& batch) -> Result<void> {
        auto put = batch.put(band, node, grid::Tree<grid::Material>{levels, material, false});
        if (!put) {
            return std::unexpected{put.error()};
        }
        return {};
    });
    REQUIRE(written.has_value());
}

} // namespace hog::testing
