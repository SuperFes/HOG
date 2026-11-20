#include "ground_cells.hpp"

#include <hog/core/hash.hpp>

#include <bit>
#include <cstdint>

namespace hog::globe {

std::size_t GroundKeyHash::operator()(const GroundKey& key) const noexcept {
    return static_cast<std::size_t>(hash_values(index_of(key.cell), key.cell.level,
                                                std::bit_cast<std::uint64_t>(key.lod),
                                                static_cast<std::uint8_t>(key.crags)));
}

std::size_t CellHash::operator()(const Cell& cell) const noexcept {
    return static_cast<std::size_t>(hash_values(index_of(cell), cell.level));
}

} // namespace hog::globe
