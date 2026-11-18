#pragma once

#include <hog/client/shape_mesh.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/key.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace hog::welding {

using client::ShapeMesh;

// Points welded where they lie within a millimetre.
class Welder {
public:
    std::uint32_t weld(hog::DVec3 point) {
        const hog::DVec3 scaled = point / tolerance;
        const std::array<long, 3> cell{std::lround(scaled.x), std::lround(scaled.y),
                                       std::lround(scaled.z)};
        for (long dx = -1; dx <= 1; ++dx) {
            for (long dy = -1; dy <= 1; ++dy) {
                for (long dz = -1; dz <= 1; ++dz) {
                    if (const auto found =
                            near({cell[0] + dx, cell[1] + dy, cell[2] + dz}, point)) {
                        return *found;
                    }
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(points_.size());
        points_.push_back(point);
        cells_[cell].push_back(index);
        return index;
    }
    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }

private:
    static constexpr double tolerance = 1e-3;

    [[nodiscard]] std::optional<std::uint32_t> near(const std::array<long, 3>& cell,
                                                    hog::DVec3 point) const {
        const auto found = cells_.find(cell);
        if (found == cells_.end()) {
            return std::nullopt;
        }
        for (const std::uint32_t index : found->second) {
            if (glm::length(points_[index] - point) < tolerance) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::vector<hog::DVec3> points_;
    std::map<std::array<long, 3>, std::vector<std::uint32_t>> cells_;
};

// The meshes as one, their vertices welded where they meet, and triangles welded down to
// nothing left out.
inline ShapeMesh welded(const std::vector<ShapeMesh>& meshes) {
    ShapeMesh one;
    Welder welder;
    const auto weld = [&](hog::DVec3 point) {
        const std::uint32_t index = welder.weld(point);
        if (index == one.vertices.size()) {
            one.vertices.push_back({.position = hog::Vec3{point}});
        }
        return index;
    };
    for (const ShapeMesh& mesh : meshes) {
        std::vector<std::uint32_t> to;
        to.reserve(mesh.vertices.size());
        for (const auto& vertex : mesh.vertices) {
            to.push_back(weld(mesh.origin + hog::DVec3{vertex.position}));
        }
        for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
            const std::uint32_t a = to[mesh.indices[at]];
            const std::uint32_t b = to[mesh.indices[at + 1]];
            const std::uint32_t c = to[mesh.indices[at + 2]];
            if (a != b && b != c && c != a) {
                one.indices.insert(one.indices.end(), {a, b, c});
            }
        }
    }
    return one;
}

// The eight children of `parent` drawn as blocks, those in `split` as their own eight, each
// block's finer sides where a split one is beside it.
inline std::vector<client::Block> blocks_of(grid::Key parent, std::span<const grid::Key> split,
                                            std::uint8_t cells) {
    std::vector<client::Block> blocks;
    for (std::size_t octant = 0; octant < 8; ++octant) {
        const grid::Key child = parent.child(octant);
        if (std::ranges::find(split, child) == split.end()) {
            blocks.push_back({.node = child, .cells = cells, .finer = 0});
            continue;
        }
        for (std::size_t inner = 0; inner < 8; ++inner) {
            blocks.push_back({.node = child.child(inner), .cells = cells, .finer = 0});
        }
    }
    for (client::Block& block : blocks) {
        if (block.node.depth != parent.depth + 1) {
            continue;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const std::size_t side : {0U, 1U}) {
                IVec3 step{0};
                step[axis] = side == 0 ? -1 : 1;
                const auto next = block.node.beside(step.x, step.y, step.z);
                if (next && std::ranges::find(split, *next) != split.end()) {
                    block.finer =
                        static_cast<std::uint8_t>(block.finer | (1U << ((2 * axis) + side)));
                }
            }
        }
    }
    return blocks;
}

} // namespace hog::welding
