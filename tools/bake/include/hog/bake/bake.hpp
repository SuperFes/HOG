#pragma once

#include <hog/baked/node.hpp>
#include <hog/baked/store.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hog::bake {

// A block is made and written whole: a tile this many levels coarser than the voxels baked, so
// 8 × 8 tiles of them across.
inline constexpr int block_above = 3;

// The blocks of a bake at `level` (each voxel 2^level of the finest a side): every one in the
// shell over a face, or those over a square `metres` across about `direction`.
[[nodiscard]] std::vector<globe::TileKey> face_blocks(const globe::Generator& generator,
                                                      std::uint8_t face, std::uint8_t level);
[[nodiscard]] std::vector<globe::TileKey> blocks_around(const globe::Generator& generator,
                                                        DVec3 direction, double metres,
                                                        std::uint8_t level);

// The node of the store a tile stands for.
[[nodiscard]] baked::Node node_of(globe::TileKey key) noexcept;

// The voxels of `block` at `level`, made as the server makes tiles: from the block down, a level
// at a time, each tile split until it's at `level` or nothing of it shows from outside, which
// stays as coarse as it was made. Reads nothing else.
[[nodiscard]] globe::VoxelTree make_block(const globe::Generator& generator, globe::TileKey block,
                                          std::uint8_t level);

struct Baked {
    std::size_t made = 0;    // blocks made and written
    std::size_t skipped = 0; // already baked at least as finely
    std::size_t bytes = 0;   // of pages written
};

// Called after each batch with the blocks done and to do in all.
using Progress = std::function<void(std::size_t done, std::size_t total)>;

// Bakes the blocks the store doesn't hold at `level` or finer yet, `batch` at a time across the
// job system's threads, each batch written together, so a stopped bake carries on where it left
// off. Fails, writing nothing, on a store baked from another planet.
[[nodiscard]] Result<Baked> bake(const globe::Generator& generator, baked::Store& store,
                                 jobs::JobSystem& jobs, std::span<const globe::TileKey> blocks,
                                 std::uint8_t level, std::size_t batch = 64,
                                 const Progress& progress = {});

} // namespace hog::bake
