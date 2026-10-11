#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>

#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace hog::planet {

// Keeps a tile at a million samples or fewer.
inline constexpr int max_tile_depth = 10;

// Samples a tile on the grid of its descendants `depth` levels down, row by row (i fastest)
// from the corner at the lowest i and j.
template <std::invocable<CellId> F>
[[nodiscard]] std::vector<std::invoke_result_t<F&, CellId>> sample_tile(CellId tile, int depth,
                                                                        F sample) {
    HOG_ASSERT(tile.is_valid(), "invalid tile");
    HOG_ASSERT(depth >= 0 && depth <= max_tile_depth && tile.level() + depth <= max_level,
               "tile depth out of range");
    const FaceIJ origin = tile.to_face_ij();
    const std::uint32_t side = std::uint32_t{1} << static_cast<unsigned>(depth);
    std::vector<std::invoke_result_t<F&, CellId>> samples;
    samples.reserve(std::size_t{side} * side);
    for (std::uint32_t j = 0; j < side; ++j) {
        for (std::uint32_t i = 0; i < side; ++i) {
            samples.push_back(sample(CellId::from_face_ij(
                origin.face, (origin.i * side) + i, (origin.j * side) + j, origin.level + depth)));
        }
    }
    return samples;
}

// A fixed hash of the samples and their count, for golden tests.
template <std::integral T>
[[nodiscard]] constexpr std::uint64_t tile_hash(std::span<const T> samples) noexcept {
    std::uint64_t hash = hash_values(0, samples.size());
    for (const T value : samples) {
        hash = hash_combine(hash, static_cast<std::uint64_t>(value));
    }
    return hash;
}

// The fixed-point noise point for a position, at `frequency` lattice cells per unit length.
// Rounds once per coordinate, so it is as deterministic as the position itself.
[[nodiscard]] NoisePoint noise_point(DVec3 position, double frequency);

} // namespace hog::planet
