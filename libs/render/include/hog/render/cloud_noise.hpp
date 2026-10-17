#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/render/memory.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hog::render {

class Device;

// Texels a side of the clouds' noise volumes, and their mips.
inline constexpr std::uint32_t shape_noise_size = 64;
inline constexpr std::uint32_t detail_noise_size = 32;
inline constexpr std::uint32_t noise_levels = 4;

// From 0 to 1 about `at`, in a volume repeating every 1 along each axis: 1 at points scattered one
// to each of `cells`³ cells, falling to 0 a cell away from the nearest.
[[nodiscard]] double tiled_cells(DVec3 at, int cells, std::uint32_t seed) noexcept;

// Gradient noise repeating every 1 along each axis, `period` lattice cells to it: from about -1
// to 1, 0 at the lattice's corners.
[[nodiscard]] double tiled_gradient(DVec3 at, int period, std::uint32_t seed) noexcept;

// Volumes of `size` texels a side repeating across their edges, as RGBA8 texels from 0 to 255,
// x fastest, then y, then z, and each of `levels` mips after the one before, each texel of one
// the mean of the eight under it in the one before. Baked on `jobs` if given.
//
// The billows' shape: r gradient noise worn by cells, the billows themselves; g, b and a cells at
// ever finer sizes, to wear them away.
[[nodiscard]] std::vector<std::byte> shape_noise(std::uint32_t size, std::uint32_t levels,
                                                 jobs::JobSystem* jobs = nullptr);
// The edges' detail: r, g and b cells at ever finer sizes, and a their sum.
[[nodiscard]] std::vector<std::byte> detail_noise(std::uint32_t size, std::uint32_t levels,
                                                  jobs::JobSystem* jobs = nullptr);

// The noise volumes, ready to sample, repeating, as with a tiling sampler.
struct CloudNoise {
    Image shape;
    Image detail;
};

[[nodiscard]] Result<CloudNoise> make_cloud_noise(Device& device, jobs::JobSystem& jobs);

} // namespace hog::render
