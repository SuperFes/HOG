#include <hog/client/transvoxel.hpp>
#include <hog/client/transvoxel_tables.hpp>
#include <hog/core/assert.hpp>

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::client {
namespace {

// Along which axis two points of an edge lie apart.
std::size_t axis_of(IVec3 low, IVec3 high) noexcept {
    if (high.x != low.x) {
        return 0;
    }
    return high.y != low.y ? 1 : 2;
}

// Of a cell's counts in the tables.
constexpr std::size_t vertex_count(std::uint8_t counts) noexcept {
    return static_cast<std::size_t>(counts) >> 4U;
}
constexpr std::size_t triangle_count(std::uint8_t counts) noexcept {
    return static_cast<std::size_t>(counts) & 0xFU;
}

// Of a regular cell's corner, as the tables number them.
constexpr IVec3 corner_of(unsigned corner) noexcept {
    return {static_cast<int>(corner & 1U), static_cast<int>((corner >> 1U) & 1U),
            static_cast<int>((corner >> 2U) & 1U)};
}

class Mesher {
public:
    Mesher(const grid::Grid& grid, const Block& block, const BlockField& field)
        : field_{&field}, across_{1 << block.cells}, side_{static_cast<std::size_t>(across_) + 1U} {
        const int below = grid.depth() - block.node.depth;
        HOG_ASSERT(std::cmp_less_equal(block.cells, below),
                   "a block's cells no finer than the grid's");
        step_ = std::int64_t{1} << (below - block.cells);
        base_ = I64Vec3{block.node.x, block.node.y, block.node.z} * (std::int64_t{1} << below);
        mesh_.origin = grid.low(block.node);
        sample();
    }

    ShapeMesh mesh() && {
        for (int z = 0; z < across_; ++z) {
            for (int y = 0; y < across_; ++y) {
                for (int x = 0; x < across_; ++x) {
                    regular({x, y, z});
                }
            }
        }
        return std::move(mesh_);
    }

private:
    // Points are counted in half cells from the block's low corner, so the finer side's points
    // between its corners have places of their own.
    [[nodiscard]] I64Vec3 lattice(IVec3 half) const noexcept {
        return base_ + (I64Vec3{half} * step_) / std::int64_t{2};
    }
    [[nodiscard]] std::size_t corner_index(IVec3 cell) const noexcept {
        return (((static_cast<std::size_t>(cell.z) * side_) + static_cast<std::size_t>(cell.y)) *
                side_) +
               static_cast<std::size_t>(cell.x);
    }

    void sample() {
        corners_.resize(side_ * side_ * side_);
        for (int z = 0; z <= across_; ++z) {
            for (int y = 0; y <= across_; ++y) {
                for (int x = 0; x <= across_; ++x) {
                    corners_[corner_index({x, y, z})] = field_->value(lattice(IVec3{x, y, z} * 2));
                }
            }
        }
    }

    // The vertex on the edge between two points, in half cells, made once.
    std::uint32_t vertex(IVec3 a, IVec3 b) {
        const IVec3 low = glm::min(a, b);
        const IVec3 high = glm::max(a, b);
        const std::size_t axis = axis_of(low, high);
        const int length = high[axis] - low[axis];
        const std::uint64_t side = (2U * side_) - 1U;
        const std::uint64_t key =
            (((((static_cast<std::uint64_t>(low.z) * side) + static_cast<std::uint64_t>(low.y)) *
               side) +
              static_cast<std::uint64_t>(low.x)) *
             8U) +
            (axis * 2U) + static_cast<std::uint64_t>(length - 1);
        const auto [found, made] =
            vertices_.try_emplace(key, static_cast<std::uint32_t>(mesh_.vertices.size()));
        if (made) {
            const Crossed crossed = field_->crossing(lattice(low), lattice(high));
            mesh_.vertices.push_back({.position = Vec3{crossed.at - mesh_.origin},
                                      .normal = crossed.normal,
                                      .materials = crossed.materials,
                                      .share = crossed.share});
        }
        return found->second;
    }

    void regular(IVec3 cell) {
        unsigned code = 0;
        for (unsigned corner = 0; corner < 8; ++corner) {
            if (corners_[corner_index(cell + corner_of(corner))] < 0.0) {
                code |= 1U << corner;
            }
        }
        if (code == 0 || code == 0xFF) {
            return;
        }
        const transvoxel::RegularCell& data =
            transvoxel::regular_cell_data[transvoxel::regular_cell_class[code]];
        const auto& edges = transvoxel::regular_vertex_data[code];
        std::array<std::uint32_t, 12> made{};
        for (std::size_t index = 0; index < vertex_count(data.counts); ++index) {
            const unsigned ends = edges[index] & 0xFFU;
            made[index] =
                vertex((cell + corner_of(ends >> 4U)) * 2, (cell + corner_of(ends & 0xFU)) * 2);
        }
        for (std::size_t triangle = 0; triangle < triangle_count(data.counts); ++triangle) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                mesh_.indices.push_back(made[data.indices[(3 * triangle) + corner]]);
            }
        }
    }

    const BlockField* field_;
    int across_;
    std::size_t side_; // corners a side
    std::int64_t step_ = 1;
    I64Vec3 base_{0};
    std::vector<double> corners_;
    std::unordered_map<std::uint64_t, std::uint32_t> vertices_;
    ShapeMesh mesh_;
};

} // namespace

ShapeMesh mesh_block(const grid::Grid& grid, const Block& block, const BlockField& field) {
    HOG_ASSERT(block.finer == 0, "transition cells not drawn yet");
    return Mesher{grid, block, field}.mesh();
}

} // namespace hog::client
