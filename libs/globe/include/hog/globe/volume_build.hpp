#pragma once

#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>

#include <cstdint>

namespace hog::globe {

// How far under its lowest ground a tile column is made ahead, in layers.
inline constexpr int made_depth = 16;

// The tiles up the tile column at (`face`, `i`, `j`), counted in tiles, as the generator makes
// them: from `made_depth` under its lowest ground, its floor, to the highest ground or sea over
// it. Reads nothing else, so tile columns can be made side by side and merged.
[[nodiscard]] Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                                    std::uint32_t j);

// Makes a tile's height more under the tile column's floor and lowers the floor to it, keeping
// all over the old floor as it is; false where the column isn't made or is made to the bottom.
bool deepen(Volume& volume, const Generator& generator, std::uint8_t face, std::uint32_t i,
            std::uint32_t j);

} // namespace hog::globe
