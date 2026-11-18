#include <hog/client/transvoxel.hpp>
#include <hog/client/transvoxel_tables.hpp>
#include <hog/core/assert.hpp>

#include <array>
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

// How far into a cell a transition cell reaches, the regular cells beside it moved over to make
// room.
constexpr double transition_width = 0.5;

// Of a transition cell's samples on its full face, which bit of its case each takes; and of
// those on its half face, the full face's corner each copies.
constexpr std::array<unsigned, 9> sample_bit{0, 1, 2, 7, 8, 3, 6, 5, 4};
constexpr std::array<unsigned, 4> half_copies{0, 2, 6, 8};

// Of a regular cell's corner, as the tables number them.
constexpr IVec3 corner_of(unsigned corner) noexcept {
    return {static_cast<int>(corner & 1U), static_cast<int>((corner >> 1U) & 1U),
            static_cast<int>((corner >> 2U) & 1U)};
}

class Mesher {
public:
    Mesher(const grid::Grid& grid, const Block& block, const BlockField& field)
        : field_{&field}, across_{1 << block.cells}, side_{static_cast<std::size_t>(across_) + 1U},
          finer_{block.finer},
          cell_{grid.size(static_cast<std::uint8_t>(block.node.depth + block.cells))} {
        const int below = grid.depth() - block.node.depth;
        HOG_ASSERT(std::cmp_less_equal(block.cells, below),
                   "a block's cells no finer than the grid's");
        HOG_ASSERT(block.finer == 0 || std::cmp_less(block.cells, below),
                   "a finer side's cells no finer than the grid's");
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
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const bool high : {false, true}) {
                if (!is_finer(axis, high)) {
                    continue;
                }
                for (int v = 0; v < across_; ++v) {
                    for (int u = 0; u < across_; ++u) {
                        transition(axis, high, u, v);
                    }
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

    [[nodiscard]] bool is_finer(std::size_t axis, bool high) const noexcept {
        return ((finer_ >> ((2U * axis) + (high ? 1U : 0U))) & 1U) != 0U;
    }

    // The value at a point, in half cells: a corner's, or a finer side's.
    double value(IVec3 half) {
        if (half.x % 2 == 0 && half.y % 2 == 0 && half.z % 2 == 0) {
            return corners_[corner_index(half / 2)];
        }
        const auto [found, made] = between_.try_emplace(key_of(half, 0, 1), 0.0);
        if (made) {
            found->second = field_->value(lattice(half));
        }
        return found->second;
    }

    [[nodiscard]] std::uint64_t key_of(IVec3 low, std::size_t axis, int length) const noexcept {
        const std::uint64_t side = (2U * side_) - 1U;
        return (((((static_cast<std::uint64_t>(low.z) * side) + static_cast<std::uint64_t>(low.y)) *
                  side) +
                 static_cast<std::uint64_t>(low.x)) *
                8U) +
               (axis * 2U) + static_cast<std::uint64_t>(length - 1);
    }

    // Where a vertex of the regular cells goes, moved off the finer sides to make room for the
    // transition cells there; less so towards the other sides, so none on those moves and they
    // still meet the blocks beside them. Slid along the ground, so its shape holds.
    [[nodiscard]] DVec3 moved(DVec3 at, Vec3 normal) const noexcept {
        const DVec3 cells = (at - mesh_.origin) / cell_;
        DVec3 by{0.0};
        double taper = 1.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const bool high : {false, true}) {
                const double from = high ? across_ - cells[axis] : cells[axis];
                if (is_finer(axis, high)) {
                    by[axis] += (high ? -1.0 : 1.0) * transition_width * glm::max(0.0, 1.0 - from);
                } else {
                    taper *= glm::clamp(from, 0.0, 1.0);
                }
            }
        }
        by *= taper;
        if (glm::dot(normal, normal) > 0.0F) {
            const DVec3 facing = glm::normalize(DVec3{normal});
            by -= facing * glm::dot(facing, by);
        }
        return at + (by * cell_);
    }

    // The vertex on the edge between two points, in half cells, made once.
    std::uint32_t vertex(IVec3 a, IVec3 b) {
        const IVec3 low = glm::min(a, b);
        const IVec3 high = glm::max(a, b);
        const std::size_t axis = axis_of(low, high);
        const int length = high[axis] - low[axis];
        const auto [found, made] = vertices_.try_emplace(
            key_of(low, axis, length), static_cast<std::uint32_t>(mesh_.vertices.size()));
        if (made) {
            const Crossed crossed = field_->crossing(lattice(low), lattice(high));
            // Those between a finer side's points lie on it, and stay.
            const DVec3 at =
                length == 2 && finer_ != 0 ? moved(crossed.at, crossed.normal) : crossed.at;
            mesh_.vertices.push_back({.position = Vec3{at - mesh_.origin},
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

    // A point on a finer side, in half cells, `u` and `v` along the next two axes round.
    [[nodiscard]] IVec3 on_side(std::size_t axis, bool high, int u, int v) const noexcept {
        IVec3 half{0};
        half[axis] = high ? 2 * across_ : 0;
        half[(axis + 1) % 3] = u;
        half[(axis + 2) % 3] = v;
        return half;
    }

    // The transition cell over the cell at `u`, `v` along a finer side.
    void transition(std::size_t axis, bool high, int u, int v) {
        std::array<IVec3, 13> samples{};
        unsigned code = 0;
        for (unsigned sample = 0; sample < 9; ++sample) {
            samples[sample] = on_side(axis, high, (2 * u) + static_cast<int>(sample % 3),
                                      (2 * v) + static_cast<int>(sample / 3));
            if (value(samples[sample]) < 0.0) {
                code |= 1U << sample_bit[sample];
            }
        }
        for (std::size_t copy = 0; copy < 4; ++copy) {
            samples[9 + copy] = samples[half_copies[copy]];
        }
        if (code == 0 || code == 0x1FF) {
            return;
        }
        const unsigned cell_class = transvoxel::transition_cell_class[code];
        const transvoxel::TransitionCell& data =
            transvoxel::transition_cell_data[cell_class & 0x7FU];
        const auto& edges = transvoxel::transition_vertex_data[code];
        std::array<std::uint32_t, 12> made{};
        for (std::size_t index = 0; index < vertex_count(data.counts); ++index) {
            const unsigned ends = edges[index] & 0xFFU;
            made[index] = vertex(samples[ends >> 4U], samples[ends & 0xFU]);
        }
        // The next two axes round from the side's run one way seen from inside on the low side,
        // the other on the high side.
        const bool reversed = ((cell_class & 0x80U) != 0U) != high;
        for (std::size_t triangle = 0; triangle < triangle_count(data.counts); ++triangle) {
            const std::size_t first = 3 * triangle;
            mesh_.indices.push_back(made[data.indices[first]]);
            mesh_.indices.push_back(made[data.indices[first + (reversed ? 2 : 1)]]);
            mesh_.indices.push_back(made[data.indices[first + (reversed ? 1 : 2)]]);
        }
    }

    const BlockField* field_;
    int across_;
    std::size_t side_; // corners a side
    std::uint8_t finer_;
    double cell_; // metres across a cell
    std::int64_t step_ = 1;
    I64Vec3 base_{0};
    std::vector<double> corners_;
    std::unordered_map<std::uint64_t, double> between_; // by key_of, as if an edge along x
    std::unordered_map<std::uint64_t, std::uint32_t> vertices_;
    ShapeMesh mesh_;
};

} // namespace

ShapeMesh mesh_block(const grid::Grid& grid, const Block& block, const BlockField& field) {
    return Mesher{grid, block, field}.mesh();
}

} // namespace hog::client
