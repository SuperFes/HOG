#pragma once

#include <hog/core/math.hpp>
#include <hog/weather/grid.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hog::weather {

// A cell's neighbour across one of its faces: the four beside it in its layer, the layers above
// and below.
struct Link {
    static constexpr std::uint32_t none = ~std::uint32_t{0};

    std::uint32_t cell = none;
    float distance = 0.0F; // metres between the middles
    Vec3 direction{0.0F};  // unit, from this middle towards that one, level with the ground here
    std::uint8_t back = 0; // which of that cell's links lead back to this one
};

inline constexpr int links_per_cell = 6;

// The matrix that takes a sum of the open links' directions each times a value's difference over
// the link's distance to the value's gradient: the inverse of the sum of the directions' squares.
[[nodiscard]] Mat3 gradient_matrix(const std::array<Link, links_per_cell>& links,
                                   std::uint8_t closed);

// The vector `w` that gives the change in a value from a cell's middle to a point `offset` from it
// as the sum, over the links not in `closed` (a bit to a link), of the neighbour's difference over
// its distance times the link's direction dotted with `w`: the least-squares gradient's. Where the
// neighbours lie square about the cell that's half the offset along each link, but the cube
// sphere's cells are skewed towards its edges and corners, where fitting keeps the gradient the
// same whichever way it points. A direction with no neighbour open on either side is left out.
[[nodiscard]] Vec3 gradient_weights(const std::array<Link, links_per_cell>& links,
                                    std::uint8_t closed, Vec3 offset);

// Who lies beside whom, found once for a grid: what the solver steps across. Links 0 to 3 are
// the sides of the cell's column, 4 the layer above, 5 below.
class Stencil {
public:
    explicit Stencil(const Grid& grid);

    [[nodiscard]] const Grid& grid() const noexcept { return grid_; }
    [[nodiscard]] const std::array<Link, links_per_cell>& links(std::size_t cell) const {
        return links_[cell];
    }
    // The unit direction up from a column's middle.
    [[nodiscard]] Vec3 up(std::size_t column) const { return up_[column]; }

private:
    Grid grid_;
    std::vector<std::array<Link, links_per_cell>> links_;
    std::vector<Vec3> up_;
};

} // namespace hog::weather
