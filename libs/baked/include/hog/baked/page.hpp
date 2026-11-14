#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::baked {

// The mark of space nothing is known of yet, as `globe::Land` has it: coarser than any level.
inline constexpr std::uint8_t unknown = 255;

// What a part of a node holds, all alike: one voxel of the world's, and how it's known: 0 as fine
// as it goes, 1 + a level where made that coarsely with finer to come, `unknown` never made.
struct Leaf {
    globe::Cube cube;
    std::uint8_t open = 0;
    std::uint8_t size = 0; // log2 of its side, in the finest voxels

    friend constexpr bool operator==(const Leaf&, const Leaf&) noexcept = default;
};

// A node's subtree as stored, read where it lies without unpacking: the stored bytes are walked
// from the root to the leaf holding a point, one step a level.
//
// Little-endian: a 16-byte header (version, size, voxel, flags; root entry; cube and node
// counts), then the cubes, 12 bytes each, then the inner nodes, eight 4-byte entries each. An
// entry is either an inner node's index or, top bit set, a leaf: its mark in bits 23 to 30 and its
// cube's index under them. Nodes and cubes alike are stored once.
class Page {
public:
    static constexpr std::uint8_t version = 1;
    static constexpr std::size_t header_bytes = 16;
    static constexpr std::size_t cube_bytes = 12;
    static constexpr std::size_t node_bytes = 32;
    // Nothing of it shows from outside at any level: buried, or all one with no surface near.
    static constexpr std::uint8_t hidden = 1;

    // A page of nothing, to be assigned over.
    Page() = default;
    // Fails, saying why, on bytes that aren't a page.
    [[nodiscard]] static Result<Page> view(std::span<const std::byte> bytes);
    // A page only views its bytes; they must outlive it.
    static Result<Page> view(std::vector<std::byte>&& bytes) = delete;

    // log2 of its side, and of each of its voxels' sides, in the finest voxels.
    [[nodiscard]] std::uint8_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint8_t voxel() const noexcept { return voxel_; }
    [[nodiscard]] std::uint8_t flags() const noexcept { return flags_; }
    [[nodiscard]] std::uint32_t cubes() const noexcept { return cubes_; }
    [[nodiscard]] std::uint32_t nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

    // The leaf holding the point, counted in finest voxels from the node's corner.
    // Precondition: within the node.
    [[nodiscard]] Leaf at(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept;
    // The leaf the cube 2^size finest voxels a side from (x, y, z) lies in whole; none where it
    // holds more than one. Precondition: within the node, on that size's grid, no finer than a
    // voxel.
    [[nodiscard]] std::optional<Leaf> whole(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                                            std::uint8_t size) const noexcept;

private:
    explicit Page(std::span<const std::byte> bytes) : bytes_{bytes} {}
    [[nodiscard]] std::uint32_t child(std::uint32_t node, std::size_t octant) const noexcept;
    [[nodiscard]] Leaf leaf(std::uint32_t entry, std::uint8_t size) const noexcept;

    std::span<const std::byte> bytes_;
    std::uint8_t size_ = 0;
    std::uint8_t voxel_ = 0;
    std::uint8_t flags_ = 0;
    std::uint32_t root_ = 0;
    std::uint32_t cubes_ = 0;
    std::uint32_t nodes_ = 0;
};

// Gathers a page's cubes and nodes bottom up, each distinct one once.
class PageBuilder {
public:
    // An entry for a leaf, and for an inner node of eight entries: eight leaves alike are one.
    std::uint32_t leaf(const globe::Cube& cube, std::uint8_t open);
    std::uint32_t inner(const std::array<std::uint32_t, 8>& children);
    [[nodiscard]] std::vector<std::byte> finish(std::uint32_t root, std::uint8_t size,
                                                std::uint8_t voxel, std::uint8_t flags) const;

private:
    struct CubeHash {
        std::size_t operator()(const globe::Cube& cube) const noexcept;
    };
    struct NodeHash {
        std::size_t operator()(const std::array<std::uint32_t, 8>& node) const noexcept;
    };
    std::vector<globe::Cube> cubes_;
    std::unordered_map<globe::Cube, std::uint32_t, CubeHash> cube_ids_;
    std::vector<std::array<std::uint32_t, 8>> nodes_;
    std::unordered_map<std::array<std::uint32_t, 8>, std::uint32_t, NodeHash> node_ids_;
};

// A page of a node 2^size finest voxels a side, its voxels 2^voxel: `look(x, y, z, cells)`, in
// voxels, gives the leaf the cube 2^cells voxels a side from that corner lies in whole, and none
// where it holds more than one; for a single voxel it always gives one.
template <typename Look>
[[nodiscard]] std::vector<std::byte> encode(std::uint8_t size, std::uint8_t voxel,
                                            std::uint8_t flags, const Look& look) {
    PageBuilder builder;
    // A walk down, children in turn: each cube's entry is made once its eighth child's is.
    struct Walk {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 0;
        std::uint8_t cells = 0;
        std::size_t next = 0;
        std::array<std::uint32_t, 8> children{};
    };
    std::vector<Walk> pending{{.cells = static_cast<std::uint8_t>(size - voxel)}};
    std::uint32_t done = 0;
    while (!pending.empty()) {
        const Walk& walk = pending.back();
        if (walk.next == 0) {
            if (const std::optional<Leaf> whole = look(walk.x, walk.y, walk.z, walk.cells)) {
                done = builder.leaf(whole->cube, whole->open);
                pending.pop_back();
                if (!pending.empty()) {
                    pending.back().children.at(pending.back().next++) = done;
                }
                continue;
            }
        }
        if (walk.next == 8) {
            done = builder.inner(walk.children);
            pending.pop_back();
            if (!pending.empty()) {
                pending.back().children.at(pending.back().next++) = done;
            }
            continue;
        }
        // Only a cube wider than a voxel gets here: a lone voxel is always whole.
        const std::uint32_t half = std::uint32_t{1} << (walk.cells - 1U);
        const std::size_t k = walk.next;
        pending.push_back({.x = walk.x + ((k & 1U) != 0 ? half : 0U),
                           .y = walk.y + ((k & 2U) != 0 ? half : 0U),
                           .z = walk.z + ((k & 4U) != 0 ? half : 0U),
                           .cells = static_cast<std::uint8_t>(walk.cells - 1U)});
    }
    return builder.finish(done, size, voxel, flags);
}

// A page of all one leaf.
[[nodiscard]] std::vector<std::byte> encode_one(std::uint8_t size, std::uint8_t voxel,
                                                std::uint8_t flags, const globe::Cube& cube,
                                                std::uint8_t open);
// A page of a tree, each of its voxels 2^voxel finest a side. Precondition: its side is
// 2^(size - voxel).
[[nodiscard]] std::vector<std::byte> encode_tree(const globe::VoxelTree& tree, std::uint8_t size,
                                                 std::uint8_t voxel, std::uint8_t flags);
// The page of the cube 2^size finest voxels a side from (x, y, z) of another, as fine, or one
// voxel where it's within one. Precondition: within it, on that size's grid.
[[nodiscard]] std::vector<std::byte> encode_part(const Page& page, std::uint32_t x, std::uint32_t y,
                                                 std::uint32_t z, std::uint8_t size);

} // namespace hog::baked
