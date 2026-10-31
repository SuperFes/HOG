#pragma once

#include <hog/globe/hex_cell.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::globe {

// Every cell of a level by its index, with its neighbours' indices worked out once, for processes
// that step across the whole globe again and again.
class HexGrid {
public:
    explicit HexGrid(int level);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(sides_.size());
    }
    [[nodiscard]] std::span<const std::uint32_t> neighbors(std::uint32_t index) const noexcept {
        return std::span{around_[index]}.first(sides_[index]);
    }

private:
    int level_;
    std::vector<std::array<std::uint32_t, 6>> around_;
    std::vector<std::uint8_t> sides_;
};

} // namespace hog::globe
