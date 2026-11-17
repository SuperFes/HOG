#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/key.hpp>

#include <cstdint>
#include <optional>

namespace hog::grid {

// Levels a page spans: its finest nodes are 64 a side under its root.
inline constexpr std::uint8_t page_levels = 6;

// The root node round the planet, centred on its middle, and how deep its finest nodes lie.
// Points are metres from the planet's centre.
class Grid {
public:
    // The shallowest root halving down to `finest` metres that holds a ball of `reach` metres.
    // Precondition: finest > 0, reach > 0, and that root no deeper than max_depth.
    Grid(double reach, double finest);

    // Of the finest nodes.
    [[nodiscard]] std::uint8_t depth() const noexcept { return depth_; }
    [[nodiscard]] double finest() const noexcept { return finest_; }
    // Metres across a node at `at` deep.
    [[nodiscard]] double size(std::uint8_t at) const noexcept;

    [[nodiscard]] DVec3 low(Key key) const noexcept;
    [[nodiscard]] DVec3 centre(Key key) const noexcept;
    // The node at `at` deep holding `point`; none outside the root.
    [[nodiscard]] std::optional<Key> key_at(DVec3 point, std::uint8_t at) const noexcept;

    // The root of the page holding `key`. Pages are cut every page_levels up from the finest
    // depth, so the finest pages are full and the top page, rooted at the root, holds what's
    // left; a page holds the levels under its root, and the root node is the top page's.
    [[nodiscard]] Key page_of(Key key) const noexcept;
    // Levels the page rooted `at` deep holds under its root: page_levels, but the top page holds
    // what's left over the cuts, and none goes past the finest depth. Precondition: a page's root.
    [[nodiscard]] std::uint8_t levels_under(std::uint8_t at) const noexcept;

private:
    double finest_;
    std::uint8_t depth_ = 0;
};

} // namespace hog::grid
