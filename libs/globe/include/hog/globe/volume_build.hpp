#pragma once

#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>

#include <cstdint>

namespace hog::globe {

// The tiles up the tile column at (`face`, `i`, `j`), counted in tiles, as the generator makes
// them: every layer to the highest ground or sea over it. Reads nothing else, so tile columns can
// be made side by side and merged.
[[nodiscard]] Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                                    std::uint32_t j);

} // namespace hog::globe
