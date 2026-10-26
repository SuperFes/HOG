#pragma once

#include <hog/planet/cell_id.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Every cell of one level on a planet of a given radius, numbered face by face and row by row,
// with the neighbours across each edge and corner. Distances and areas are computed without
// transcendental functions, so they are the same on every platform.
class LevelGrid {
public:
    static constexpr int max_level = 10;

    // Precondition: 0 <= level <= max_level and radius > 0.
    LevelGrid(int level, double radius);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(areas_.size());
    }

    [[nodiscard]] CellId cell(std::uint32_t index) const;
    // Precondition: the cell is at the grid's level.
    [[nodiscard]] std::uint32_t index(CellId cell) const;

    // Cells sharing an edge or a corner, as vertex_neighbors gives them.
    [[nodiscard]] std::span<const std::uint32_t> neighbors(std::uint32_t index) const {
        return std::span{around_}.subspan(first_[index], first_[index + 1] - first_[index]);
    }
    // Fixed-point metres between the centres of a cell and its `k`th neighbour; exact for cells
    // under 46 km across, to within a few millimetres beyond.
    [[nodiscard]] std::int64_t distance(std::uint32_t index, std::size_t k) const {
        return std::int64_t{distances_[first_[index] + k]} << distance_shift_;
    }

    // Square metres.
    [[nodiscard]] std::int64_t area(std::uint32_t index) const { return areas_[index]; }
    // A field with one fixed-point value per grid cell at any cell's centre, bilinear between the
    // four grid cells around it.
    [[nodiscard]] std::int64_t sample(std::span<const std::int64_t> field, CellId cell) const;
    // The same, as a quadratic B-spline over the nine grid cells around the nearest: smooth in
    // value and slope, without creases along cells' edges, but short of a cell's own value at its
    // centre where the field bends.
    [[nodiscard]] std::int64_t smooth(std::span<const std::int64_t> field, CellId cell) const;
    // Those four grid cells; at a cube corner, where only three meet, the cell's own comes twice.
    [[nodiscard]] std::array<std::uint32_t, 4> corners(CellId cell) const;

private:
    // The grid cell under a point and the point's offset from its centre along u and v, in
    // 2^-shift of a cell.
    struct Offset {
        std::uint32_t here;
        std::int64_t u;
        std::int64_t v;
        int shift;
    };
    [[nodiscard]] Offset offset(CellId cell) const;

    // The four cells sample blends between, here, along u, along v and between, with the weights
    // along u and v.
    struct Blend {
        std::array<std::uint32_t, 4> cells;
        std::int64_t along_u;
        std::int64_t along_v;
    };
    [[nodiscard]] Blend blend(CellId cell) const;

    // Grid cells around one, by quadrant: the neighbour along u, along v and between the two,
    // for -u-v, +u-v, -u+v and +u+v. At a cube corner there is none between, and the third is
    // the cell itself.
    using Quadrant = std::array<std::uint32_t, 3>;
    [[nodiscard]] Quadrant quadrant(std::uint32_t here, std::size_t quadrant) const;

    int level_;
    std::uint32_t side_;
    std::vector<std::uint32_t> first_;
    std::vector<std::uint32_t> around_;
    // Kept in 32 bits, shifted down as far as the largest needs.
    std::vector<std::uint32_t> distances_;
    int distance_shift_ = 0;
    std::vector<std::int64_t> areas_;
    // Inside a face the cells around one are next to it in the numbering; only those of cells
    // on a face's edge, ordered by index, are kept.
    std::vector<std::uint32_t> edge_cells_;
    std::vector<std::array<Quadrant, 4>> edge_quadrants_;
};

} // namespace hog::planet
