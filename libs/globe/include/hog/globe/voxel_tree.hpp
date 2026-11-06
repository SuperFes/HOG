#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/cube.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::globe {

// A cube of voxels (each a `Cube`), `side` across (a power of two), held as a tree of cubes: a cube whose voxels
// are all alike is one leaf, whatever its size, and any cubes alike, children and all, are one
// node however many places they stand. Every point is still a voxel: `at` tells what is there, and
// whether it's a lone voxel or part of a larger leaf is not for the asker to know.
//
// Nodes are never changed once made; `set` makes new nodes along its path and shares the rest, and
// cubes that come alike again are one leaf again, so the same voxels are always the same tree.
// Nodes no longer reachable are not yet taken back.
class VoxelTree {
public:
    using Id = std::uint32_t;

    // A tree of `side` across, all `fill`.
    explicit VoxelTree(int side, const Cube& fill = {});

    // Builds from `voxel_at(x, y, z)`, the voxels of the cube from its corner.
    template <typename VoxelAt>
    [[nodiscard]] static VoxelTree build(int side, const VoxelAt& voxel_at) {
        VoxelTree tree{side};
        // Each level's cubes from the finer level's, joining those alike as it goes.
        std::vector<Id> level;
        level.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) *
                      static_cast<std::size_t>(side));
        for (int z = 0; z < side; ++z) {
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    level.push_back(tree.leaf_of(voxel_at(x, y, z)));
                }
            }
        }
        for (int across = side; across > 1; across /= 2) {
            const int half = across / 2;
            std::vector<Id> coarser;
            coarser.reserve(static_cast<std::size_t>(half) * static_cast<std::size_t>(half) *
                            static_cast<std::size_t>(half));
            const auto at = [&](int x, int y, int z) {
                return level[(((static_cast<std::size_t>(z) * static_cast<std::size_t>(across)) +
                               static_cast<std::size_t>(y)) *
                              static_cast<std::size_t>(across)) +
                             static_cast<std::size_t>(x)];
            };
            for (int z = 0; z < half; ++z) {
                for (int y = 0; y < half; ++y) {
                    for (int x = 0; x < half; ++x) {
                        std::array<Id, 8> children{};
                        for (std::size_t k = 0; k < 8; ++k) {
                            children.at(k) = at((2 * x) + static_cast<int>(k & 1U),
                                                (2 * y) + static_cast<int>((k >> 1U) & 1U),
                                                (2 * z) + static_cast<int>((k >> 2U) & 1U));
                        }
                        coarser.push_back(tree.joined(children));
                    }
                }
            }
            level = std::move(coarser);
        }
        tree.root_ = level.front();
        return tree;
    }

    [[nodiscard]] int side() const noexcept { return side_; }
    [[nodiscard]] Cube at(int x, int y, int z) const;
    // Sets one voxel, splitting any larger cube it's in, and joining cubes back that come alike.
    void set(int x, int y, int z, const Cube& voxel);

    // The size of the leaf holding a point, in voxels across.
    [[nodiscard]] int leaf_side(int x, int y, int z) const;

    // Nodes reachable from the root, each counted once however often it is shared, and as if none
    // were: what the tree costs, and what it would cost with no sharing.
    [[nodiscard]] std::size_t nodes() const;
    [[nodiscard]] std::size_t unshared_nodes() const;
    [[nodiscard]] std::size_t leaves() const;

    // The tree as bytes for storing or sending: each distinct node once, children before parents.
    // The same voxels are always the same bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a tree: cut short, too large or out of range.
    [[nodiscard]] static Result<VoxelTree> decode(std::span<const std::byte> bytes);

    // Whether two trees hold the same voxels. Equal voxels make equal roots only within one tree
    // storage, so this reads the nodes.
    [[nodiscard]] bool same_as(const VoxelTree& other) const;

private:
    struct Node {
        bool leaf = true;
        Cube voxel;
        std::array<Id, 8> child{};
    };
    struct Hash {
        std::size_t operator()(const Node& node) const noexcept;
    };
    struct Equal {
        bool operator()(const Node& a, const Node& b) const noexcept;
    };

    // The id of a node alike to this one, made if there's none.
    Id intern(const Node& node);
    [[nodiscard]] Id leaf_of(const Cube& voxel);
    [[nodiscard]] Id with(Id node, int side, int x, int y, int z, const Cube& voxel);

    // The leaf the eight children are, if they are all one; else a node of them.
    [[nodiscard]] Id joined(const std::array<Id, 8>& children);

    int side_ = 1;
    Id root_ = 0;
    std::vector<Node> nodes_;
    std::unordered_map<Node, Id, Hash, Equal> ids_;
};

} // namespace hog::globe
