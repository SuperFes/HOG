#pragma once

#include <hog/planet/cell_id.hpp>

#include <array>
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
    // Fixed-point metres between the centres, in the order of `neighbors`.
    [[nodiscard]] std::span<const std::int64_t> distances(std::uint32_t index) const {
        return std::span{distances_}.subspan(first_[index], first_[index + 1] - first_[index]);
    }

    // Square metres.
    [[nodiscard]] std::int64_t area(std::uint32_t index) const { return areas_[index]; }
    // A field with one fixed-point value per grid cell at any cell's centre, bilinear between the
    // four grid cells around it.
    [[nodiscard]] std::int64_t sample(std::span<const std::int64_t> field, CellId cell) const;
    // Those four grid cells; at a cube corner, where only three meet, the cell's own comes twice.
    [[nodiscard]] std::array<std::uint32_t, 4> corners(CellId cell) const;

private:
    // The four cells sample blends between, here, along u, along v and between, with the weights
    // along u and v.
    struct Blend {
        std::array<std::uint32_t, 4> cells;
        std::int64_t along_u;
        std::int64_t along_v;
    };
    [[nodiscard]] Blend blend(CellId cell) const;

    // Grid cells around each one, by quadrant: the neighbour along u, along v and between the
    // two, for -u-v, +u-v, -u+v and +u+v. At a cube corner there is none between, and the third
    // is the cell itself.
    using Quadrant = std::array<std::uint32_t, 3>;

    int level_;
    std::uint32_t side_;
    std::vector<std::uint32_t> first_;
    std::vector<std::uint32_t> around_;
    std::vector<std::int64_t> distances_;
    std::vector<std::int64_t> areas_;
    std::vector<std::array<Quadrant, 4>> quadrants_;
};

} // namespace hog::planet
