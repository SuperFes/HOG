#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/cube.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::globe {

// A cube of voxels (each a `Cube`), `side` across (a power of two), held as a tree of cubes: a cube
// whose voxels are all alike is one leaf, whatever its size, and any cubes alike, children and all,
// are one node however many places they stand. Every point is still a voxel: `at` tells what is
// there, and whether it's a lone voxel or part of a larger leaf is not for the asker to know.
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

    // Builds from the top down: `whole(x, y, z, across)` gives what every voxel of the cube
    // `across` wide from that corner is, where it can tell, and `voxel_at` the rest one by one.
    // The same tree as building from every voxel.
    template <typename Whole, typename VoxelAt>
    [[nodiscard]] static VoxelTree build_down(int side, const Whole& whole,
                                              const VoxelAt& voxel_at) {
        VoxelTree tree{side};
        tree.root_ = tree.made(0, 0, 0, side, whole, voxel_at);
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

    template <typename Whole, typename VoxelAt>
    [[nodiscard]] Id made(int x, int y, int z, int across, const Whole& whole,
                          const VoxelAt& voxel_at) {
        // Cubes still being split, each with its children made so far; a finished cube's id goes
        // to its parent, or is the answer once none are left.
        struct Open {
            int x = 0;
            int y = 0;
            int z = 0;
            int across = 0;
            std::size_t next = 0;
            std::array<Id, 8> children{};
        };
        std::vector<Open> open;
        const auto done = [&](int at_x, int at_y, int at_z, int at_across) -> std::optional<Id> {
            if (const auto all = whole(at_x, at_y, at_z, at_across)) {
                return leaf_of(*all);
            }
            if (at_across == 1) {
                return leaf_of(voxel_at(at_x, at_y, at_z));
            }
            return std::nullopt;
        };
        if (const auto id = done(x, y, z, across)) {
            return *id;
        }
        open.push_back({.x = x, .y = y, .z = z, .across = across});
        while (true) {
            Open& top = open.back();
            if (top.next == 8) {
                const Id id = joined(top.children);
                open.pop_back();
                if (open.empty()) {
                    return id;
                }
                open.back().children.at(open.back().next++) = id;
                continue;
            }
            const int half = top.across / 2;
            const int at_x = top.x + (half * static_cast<int>(top.next & 1U));
            const int at_y = top.y + (half * static_cast<int>((top.next >> 1U) & 1U));
            const int at_z = top.z + (half * static_cast<int>((top.next >> 2U) & 1U));
            if (const auto id = done(at_x, at_y, at_z, half)) {
                top.children.at(top.next++) = *id;
            } else {
                open.push_back({.x = at_x, .y = at_y, .z = at_z, .across = half});
            }
        }
    }

    // The leaf the eight children are, if they are all one; else a node of them.
    [[nodiscard]] Id joined(const std::array<Id, 8>& children);

    int side_ = 1;
    Id root_ = 0;
    std::vector<Node> nodes_;
    std::unordered_map<Node, Id, Hash, Equal> ids_;
};

// The tree half as wide, each voxel standing for the eight it covers: of each of its solid, grain
// and liquid, as much as the eight hold on average, of the material holding most of it; their
// heat on average, and the condition of the least whole. Precondition: wider than one voxel.
[[nodiscard]] VoxelTree halved(const VoxelTree& tree);

} // namespace hog::globe
